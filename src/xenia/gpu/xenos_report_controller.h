/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_XENOS_REPORT_CONTROLLER_H_
#define XENIA_GPU_XENOS_REPORT_CONTROLLER_H_

#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace xe {
namespace gpu {

// Sequences ZPD (occlusion query) report writebacks.
//
// Guest report memory is updated directly when the report event fires. On the
// host, results take at least a command list boundary to land, often several
// frames. The guest has usually reused the slot by then. Results can also span
// multiple host query segments across submissions.
//
// Two rules at retirement: FIFO within a slot (a later write can't jump an
// older pending one), and stale generation discard (each BeginReport bumps
// the slot's sequence. Writes with a stale sequence are dropped so old host
// results don't reach a slot that's already been recycled).
//
// TODO(boma): QueryBatch Lock/Unlock is a separate model - not supported yet.
class XenosReportController {
 public:
  using ReportHandle = uint32_t;
  static constexpr ReportHandle kInvalidReportHandle = 0;

  struct BeginReportResult {
    ReportHandle report_handle = kInvalidReportHandle;
    uint32_t begin_value = 0;
  };

  using CommitGuestReportCallback = void (*)(ReportHandle report_handle,
                                             uint32_t slot_base,
                                             uint32_t begin_value,
                                             uint32_t delta_value,
                                             void* callback_context);

  explicit XenosReportController(
      CommitGuestReportCallback commit_guest_report_callback,
      void* callback_context)
      : commit_guest_report_callback_(commit_guest_report_callback),
        callback_context_(callback_context) {}

  // Bumps the slot sequence (invalidates pending writes from prior lifetime)
  // and snapshots begin_value from slot_values_.
  BeginReportResult BeginReport(uint32_t report_address);

  // Queues a pending write. Deque order preserves FIFO within the slot.
  void QueueReportWrite(uint32_t report_address, ReportHandle report_handle);

  void SetReportResolved(ReportHandle report_handle, uint32_t delta_value);

  // Retires resolved writes in FIFO order and fires the writeback callback.
  // Callback runs outside the lock.
  uint32_t RetireReports();

  // Used by strict mode to check if a result is still pending.
  bool HasQueuedWriteForAddress(uint32_t report_address) const;

  struct Stats {
    uint64_t writes_retired = 0;
  };

  const Stats& stats() const { return stats_; }
  void ResetStats();

 private:
  // Built under lock, flushed outside so writes to guest don't hold it.
  struct PendingGuestCommit {
    ReportHandle report_handle = kInvalidReportHandle;
    uint32_t slot_base = 0;
    uint32_t begin_value = 0;
    uint32_t delta_value = 0;
  };

  struct LogicalReportState {
    uint32_t slot_base = 0;
    uint64_t slot_sequence_id = 0;
    // Snapshotted at BEGIN, not at retirement. Guest will have reused by then.
    uint32_t begin_value = 0;
    uint32_t delta_value = 0;
    bool resolved = false;
  };

  // Maintains FIFO ordering within the slot.
  struct QueuedReportWrite {
    ReportHandle report_handle = kInvalidReportHandle;
    uint32_t slot_base = 0;
  };

  void ProcessReportWritesLocked(
      std::vector<PendingGuestCommit>& pending_guest_commits);

  bool HasQueuedWriteForSlotLocked(uint32_t slot_base) const;

  CommitGuestReportCallback commit_guest_report_callback_ = nullptr;
  void* callback_context_ = nullptr;

  mutable std::mutex mutex_;

  std::deque<QueuedReportWrite> queued_report_writes_;
  std::unordered_map<uint32_t, uint32_t> queued_report_write_slot_counts_;
  std::unordered_map<ReportHandle, LogicalReportState> logical_reports_;

  // Bumped at each BeginReport. Stale generation writes get discarded.
  std::unordered_map<uint32_t, uint64_t> slot_sequences_;

  // Running total per slot. Seeded into begin_value at BEGIN, advanced at
  // each retirement so counts accumulate, same as real hardware.
  std::unordered_map<uint32_t, uint32_t> slot_values_;

  Stats stats_;
  ReportHandle next_report_handle_ = 1;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_XENOS_REPORT_CONTROLLER_H_
