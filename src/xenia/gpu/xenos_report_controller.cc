/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/xenos_report_controller.h"

#include <unordered_set>

#include "xenia/base/logging.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/xenos_zpd_report.h"

namespace xe {
namespace gpu {

XenosReportController::BeginReportResult XenosReportController::BeginReport(
    uint32_t report_address) {
  std::lock_guard<std::mutex> lock(mutex_);

  uint32_t slot_base = XenosZPDReport::GetSlotBase(report_address);
  if (!slot_base) {
    return {};
  }

  // Bumping before state creation invalidates pending writes from old lifetime.
  uint64_t slot_sequence_id = ++slot_sequences_[slot_base];

  ReportHandle report_handle = next_report_handle_++;
  if (report_handle == kInvalidReportHandle) {
    // 0 is reserved as the invalid handle. Skip over it if the counter wraps.
    report_handle = next_report_handle_++;
  }

  LogicalReportState& report_state = logical_reports_[report_handle];

  report_state.slot_base = slot_base;
  report_state.slot_sequence_id = slot_sequence_id;
  report_state.begin_value = slot_values_[slot_base];
  report_state.resolved = false;
  report_state.delta_value = 0;

  return {report_handle, report_state.begin_value};
}

void XenosReportController::QueueReportWrite(uint32_t report_address,
                                             ReportHandle report_handle) {
  std::lock_guard<std::mutex> lock(mutex_);

  uint32_t slot_base = XenosZPDReport::GetSlotBase(report_address);
  if (!slot_base) {
    return;
  }

  auto existing_report = logical_reports_.find(report_handle);
  if (existing_report == logical_reports_.end()) {
    return;
  }

  QueuedReportWrite queued_write;
  queued_write.report_handle = report_handle;
  queued_write.slot_base = slot_base;

  // Appended to back, preserves FIFO so a later write can't jump an earlier one
  queued_report_writes_.push_back(queued_write);
  ++queued_report_write_slot_counts_[queued_write.slot_base];
}

void XenosReportController::SetReportResolved(ReportHandle report_handle,
                                              uint32_t delta_value) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto existing_report = logical_reports_.find(report_handle);
  if (existing_report == logical_reports_.end()) {
    return;
  }

  existing_report->second.resolved = true;

  existing_report->second.delta_value = delta_value;
}

uint32_t XenosReportController::RetireReports() {
  std::vector<PendingGuestCommit> pending_guest_commits;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ProcessReportWritesLocked(pending_guest_commits);
  }

  // Fire callbacks outside the lock. The callback writes directly to guest
  // memory and may call back into the backend.
  if (commit_guest_report_callback_) {
    for (const PendingGuestCommit& commit : pending_guest_commits) {
      commit_guest_report_callback_(commit.report_handle, commit.slot_base,
                                    commit.begin_value, commit.delta_value,
                                    callback_context_);
    }
  }

  return static_cast<uint32_t>(pending_guest_commits.size());
}

bool XenosReportController::HasQueuedWriteForAddress(
    uint32_t report_address) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return HasQueuedWriteForSlotLocked(
      XenosZPDReport::GetSlotBase(report_address));
}

void XenosReportController::ResetStats() {
  std::lock_guard<std::mutex> lock(mutex_);
  stats_ = {};
}

void XenosReportController::ProcessReportWritesLocked(
    std::vector<PendingGuestCommit>& pending_guest_commits) {
  if (queued_report_writes_.empty()) {
    return;
  }

  std::unordered_set<uint32_t> blocked_slot_bases;

  auto remove_queued_write = [this](uint32_t slot_base) {
    auto queued_write_count = queued_report_write_slot_counts_.find(slot_base);
    if (queued_write_count == queued_report_write_slot_counts_.end()) {
      return;
    }
    if (--queued_write_count->second == 0) {
      queued_report_write_slot_counts_.erase(queued_write_count);
    }
  };

  auto write_it = queued_report_writes_.begin();
  while (write_it != queued_report_writes_.end()) {
    QueuedReportWrite& queued_write = *write_it;

    auto existing_report = logical_reports_.find(queued_write.report_handle);
    if (existing_report == logical_reports_.end()) {
      if (cvars::occlusion_query_log) {
        XELOGI(
            "ZPD: Controller ProcessReportWritesLocked drop missing handle={}",
            queued_write.report_handle);
      }
      write_it = queued_report_writes_.erase(write_it);
      remove_queued_write(queued_write.slot_base);
    } else {
      LogicalReportState& report_state = existing_report->second;

      // Block unresolved later writes in the same slot, preserving FIFO order.
      if (!report_state.resolved) {
        blocked_slot_bases.insert(queued_write.slot_base);
        ++write_it;
      } else if (blocked_slot_bases.count(queued_write.slot_base)) {
        if (cvars::occlusion_query_log) {
          XELOGI(
              "ZPD: Controller ProcessReportWritesLocked blocked handle={} "
              "slot=0x{:08X}",
              queued_write.report_handle, queued_write.slot_base);
        }
        ++write_it;
      } else {
        auto seq_it = slot_sequences_.find(report_state.slot_base);
        uint64_t current_seq =
            seq_it != slot_sequences_.end() ? seq_it->second : 0;

        // The slot has been reused since this write was queued. Discard.
        if (current_seq != report_state.slot_sequence_id) {
          if (cvars::occlusion_query_log) {
            XELOGI(
                "ZPD: Controller ProcessReportWritesLocked stale handle={} "
                "slot=0x{:08X} report_seq={} current_seq={}",
                queued_write.report_handle, queued_write.slot_base,
                report_state.slot_sequence_id, current_seq);
          }
          write_it = queued_report_writes_.erase(write_it);
          remove_queued_write(queued_write.slot_base);
          logical_reports_.erase(existing_report);
        } else {
          pending_guest_commits.push_back(
              {queued_write.report_handle, queued_write.slot_base,
               report_state.begin_value, report_state.delta_value});

          // Advance running total so next BeginReport picks up the right
          // begin_value.
          uint64_t end_value = static_cast<uint64_t>(report_state.begin_value) +
                               static_cast<uint64_t>(report_state.delta_value);

          slot_values_[report_state.slot_base] =
              end_value > UINT32_MAX ? UINT32_MAX
                                     : static_cast<uint32_t>(end_value);

          write_it = queued_report_writes_.erase(write_it);
          remove_queued_write(queued_write.slot_base);
          logical_reports_.erase(existing_report);
          ++stats_.writes_retired;
        }
      }
    }
  }
}

bool XenosReportController::HasQueuedWriteForSlotLocked(
    uint32_t slot_base) const {
  return queued_report_write_slot_counts_.find(slot_base) !=
         queued_report_write_slot_counts_.end();
}

}  // namespace gpu
}  // namespace xe
