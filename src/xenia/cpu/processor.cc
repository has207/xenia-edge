/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/processor.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/atomic.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/module.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/stack_walker.h"
#include "xenia/cpu/thread.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"

// TODO(benvanik): based on compiler support
#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend.h"
#elif XE_ARCH_ARM64
#include "xenia/cpu/backend/a64/a64_backend.h"
#endif

namespace xe {
namespace cpu {

using namespace xe::literals;

class BuiltinModule : public Module {
 public:
  explicit BuiltinModule(Processor* processor)
      : Module(processor), name_("builtin") {}

  const std::string& name() const override { return name_; }
  bool is_executable() const override { return false; }

  bool ContainsAddress(uint32_t address) override {
    return (address & 0xFFFFFFF0) == 0xFFFFFFF0;
  }

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override {
    return std::unique_ptr<Function>(new BuiltinFunction(this, address));
  }

 private:
  std::string name_;
};

class DynamicCodeModule : public Module {
 public:
  explicit DynamicCodeModule(Processor* processor)
      : Module(processor), name_("dynamic") {}

  const std::string& name() const override { return name_; }
  bool is_executable() const override { return false; }

  // User mode addresses alias physical memory, so ask about the kernel address.
  bool ContainsAddress(uint32_t address) override {
    const uint32_t kernel_address = memory_->UserModeKernelAddress(address);
    auto heap = memory_->LookupHeap(kernel_address);
    if (!heap) {
      return false;
    }
    uint32_t protect = 0;
    if (heap->QueryProtect(kernel_address, &protect) &&
        (protect & kMemoryProtectRead)) {
      return true;
    }
    // The physical windows alias each other, the parent heap records them all.
    if (heap->heap_type() != HeapType::kGuestPhysical) {
      return false;
    }
    auto parent = static_cast<PhysicalHeap*>(heap)->parent_heap();
    const uint32_t physical_address =
        memory_->GetPhysicalAddress(kernel_address);
    protect = 0;
    return parent && physical_address != UINT32_MAX &&
           parent->QueryProtect(physical_address, &protect) &&
           (protect & kMemoryProtectRead);
  }

  // Through the physical view, whose protection is what ContainsAddress
  // checks.
  const uint8_t* TranslateCode(uint32_t address) const override {
    return memory_->TranslateForRead(memory_->UserModeKernelAddress(address));
  }

  // A call is taken to return unless the code after it is a known entry. XeFu
  // packs the stubs of x86 CALLs back to back, each ending in a bl to its
  // dispatcher that never returns, and the next stub's entry is declared by
  // the call to it. An entry left from older code there only splits the
  // function, since the code after the call still runs next.
  bool EndsBeforeCallReturn(uint32_t address) override {
    return LookupSymbol(address, false) != nullptr;
  }

  // Dynamic code has no .pdata so only the swept range bounds the scan. It's
  // used only where the range ends on an unconditional branch. A range that
  // covers part of a function, like a patch or an old sweep of reused memory,
  // rarely does and doesn't cut the function off. A branch that links counts
  // too: XeFu ends its blocks with a bl that never returns. Taken when the
  // function is compiled, since a call can declare it before its code is
  // written.
  void BoundFunction(GuestFunction* function) {
    const uint32_t address = function->address();
    const uint32_t end = processor_->SweptCodeEnd(address);
    const uint32_t last = ((end + 3) & ~uint32_t(3)) - 4;
    if (end && last > address && ContainsAddress(last) &&
        IsUnconditionalBranch(
            xe::load_and_swap<uint32_t>(TranslateCode(last)))) {
      function->set_end_address(last);
    }
  }

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override {
    return processor_->backend()->CreateGuestFunction(this, address);
  }

 private:
  // b, blr or bctr, with or without link.
  static bool IsUnconditionalBranch(uint32_t code) {
    switch (code >> 26) {
      case 18:
        return true;
      case 19: {
        const uint32_t extended = (code >> 1) & 0x3FF;
        const uint32_t bo = (code >> 21) & 0x1F;
        return (extended == 16 || extended == 528) && (bo & 0x14) == 0x14;
      }
      default:
        return false;
    }
  }

  std::string name_;
};

Processor::Processor(xe::Memory* memory, ExportResolver* export_resolver)
    : memory_(memory), export_resolver_(export_resolver) {}

Processor::~Processor() {
  {
    auto global_lock = global_critical_region_.Acquire();
    modules_.clear();
    dynamic_code_module_.reset();
  }

  frontend_.reset();
  backend_.reset();

  if (trace_counts_dump_section_) {
    Profiler::UnregisterDumpSection(trace_counts_dump_section_);
    trace_counts_dump_section_ = 0;
  }

  // Backend teardown above took the generated code with it, so nothing can
  // still be counting.
  {
    std::lock_guard<std::mutex> lock(trace_counts_mutex_);
    for (auto& arena : trace_counts_arenas_) {
      xe::memory::DeallocFixed(arena.base, 0,
                               xe::memory::DeallocationType::kRelease);
    }
    trace_counts_arenas_.clear();
    trace_counts_free_.clear();
    trace_counts_regions_.clear();
    trace_counts_fallback_ = nullptr;
  }
}

bool Processor::Setup(std::unique_ptr<backend::Backend> backend) {
  debug_info_flags_ = 0;

  auto frontend = std::make_unique<ppc::PPCFrontend>(this);
  // TODO(benvanik): set options/etc.

  // Must be initialized by subclass before calling into this.
  assert_not_null(memory_);

  std::unique_ptr<Module> builtin_module(new BuiltinModule(this));
  builtin_module_ = builtin_module.get();
  modules_.push_back(std::move(builtin_module));

  if (frontend_ || backend_) {
    return false;
  }

  if (!backend) {
    return false;
  }
  if (!backend->Initialize(this)) {
    return false;
  }
  if (!frontend->Initialize()) {
    return false;
  }

  backend_ = std::move(backend);
  frontend_ = std::move(frontend);

  // Stack walker is used when profiling, debugging, and dumping.
  // Note that creation may fail, in which case we'll have to disable those
  // features.
  // The code cache may be unavailable in case of a "null" backend.
  cpu::backend::CodeCache* code_cache = backend_->code_cache();
  if (code_cache) {
    stack_walker_ = StackWalker::Create(code_cache);
  }
  RefreshTraceCountsEnabled();

  return true;
}

// Re-read at every title launch, because per-title config is applied after
// this processor was built and a first launch never re-runs Setup. Turning
// coverage off again is ignored, counting code already generated keeps
// writing to its arena and dropping it would only lose counts.
void Processor::RefreshTraceCountsEnabled() {
  if (!cvars::trace_function_coverage || trace_counts_enabled_) {
    return;
  }
  trace_counts_enabled_ = true;
  if (!trace_counts_dump_section_) {
    trace_counts_dump_section_ =
        Profiler::RegisterDumpSection([this](FILE* f) { DumpTraceCounts(f); });
  }
  // Counters accumulate until reset, so the timers have to as well or the two
  // halves of the dump would describe different windows.
  Profiler::ResetAggregation();
}

// Reads the shared retired totals plus every live arena without disturbing
// them. Racing increments are benign, an in-flight count lands next dump.
void Processor::DumpTraceCounts(FILE* f) {
  std::lock_guard<std::mutex> lock(trace_counts_mutex_);

  struct Row {
    uint32_t address;
    uint32_t instruction_count;
    uint64_t executed;
    uint32_t hottest_address;
    uint64_t hottest;
    size_t region;
  };
  std::vector<Row> rows;
  rows.reserve(trace_counts_regions_.size());
  uint64_t grand_total = 0;
  for (size_t r = 0; r < trace_counts_regions_.size(); ++r) {
    auto& region = trace_counts_regions_[r];
    Row row = {};
    row.address = region.start_address;
    row.instruction_count = static_cast<uint32_t>(region.count);
    row.region = r;
    for (size_t i = 0; i < region.count; ++i) {
      uint64_t total = region.retired[i];
      for (auto& arena : trace_counts_arenas_) {
        total +=
            reinterpret_cast<const uint64_t*>(arena.base + region.offset)[i];
      }
      row.executed += total;
      if (total > row.hottest) {
        row.hottest = total;
        row.hottest_address =
            region.start_address + static_cast<uint32_t>(i * 4);
      }
    }
    if (row.executed) {
      grand_total += row.executed;
      rows.push_back(row);
    }
  }
  if (rows.empty()) {
    return;
  }
  std::sort(rows.begin(), rows.end(),
            [](const Row& a, const Row& b) { return a.executed > b.executed; });

  std::fprintf(f, "\n\nguestcoverage,%llu\n",
               static_cast<unsigned long long>(grand_total));
  std::fprintf(f, "address,instructions,executed,share,hottest,hottestcount\n");
  for (auto& row : rows) {
    std::fprintf(
        f, "\"%08X\",%u,%llu,%.6f,\"%08X\",%llu\n", row.address,
        row.instruction_count, static_cast<unsigned long long>(row.executed),
        double(row.executed) / double(grand_total), row.hottest_address,
        static_cast<unsigned long long>(row.hottest));
  }

  // Long format so the column count does not depend on how many guest threads
  // happen to be live. Threads that already exited share the retired row.
  std::fprintf(f, "\n\nguestcoveragethreads\n");
  std::fprintf(f, "address,thread,executed\n");
  for (auto& row : rows) {
    auto& region = trace_counts_regions_[row.region];
    uint64_t retired = 0;
    for (size_t i = 0; i < region.count; ++i) {
      retired += region.retired[i];
    }
    if (retired) {
      std::fprintf(f, "\"%08X\",\"retired\",%llu\n", row.address,
                   static_cast<unsigned long long>(retired));
    }
    for (auto& arena : trace_counts_arenas_) {
      auto counts =
          reinterpret_cast<const uint64_t*>(arena.base + region.offset);
      uint64_t executed = 0;
      for (size_t i = 0; i < region.count; ++i) {
        executed += counts[i];
      }
      if (executed) {
        std::fprintf(f, "\"%08X\",\"%08X\",%llu\n", row.address,
                     arena.thread_id,
                     static_cast<unsigned long long>(executed));
      }
    }
  }

  DumpSequences(f);
}

// Which backend sequence the emitted code actually spends its executions in.
// Every key maps to exactly one selection in the backend's sequence table, so
// the top rows name the emitters worth optimizing for this title.
void Processor::DumpSequences(FILE* f) {
  struct SequenceRow {
    uint64_t executed = 0;
    uint64_t occurrences = 0;
    uint64_t host_bytes = 0;
  };
  std::map<uint64_t, SequenceRow> sequences;
  std::vector<uint64_t> totals;
  uint64_t grand_total = 0;
  for (auto& region : trace_counts_regions_) {
    if (region.samples.empty()) {
      continue;
    }
    totals.assign(region.count, 0);
    for (size_t i = 0; i < region.count; ++i) {
      uint64_t total = region.retired[i];
      for (auto& arena : trace_counts_arenas_) {
        total +=
            reinterpret_cast<const uint64_t*>(arena.base + region.offset)[i];
      }
      totals[i] = total;
    }
    for (auto& sample : region.samples) {
      if (sample.guest_index >= region.count) {
        continue;
      }
      auto& row = sequences[sample.key];
      row.executed += totals[sample.guest_index];
      row.occurrences += 1;
      row.host_bytes += sample.host_bytes;
      grand_total += totals[sample.guest_index];
    }
  }
  if (!grand_total) {
    return;
  }

  std::vector<std::pair<uint64_t, SequenceRow>> rows(sequences.begin(),
                                                     sequences.end());
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    return a.second.executed > b.second.executed;
  });

  std::fprintf(f, "\n\nguestsequences,%llu\n",
               static_cast<unsigned long long>(grand_total));
  std::fprintf(f, "key,sequence,executed,share,occurrences,avgbytes\n");
  for (auto& row : rows) {
    std::string label =
        backend_ ? backend_->FormatSequenceKey(row.first) : std::string();
    std::fprintf(
        f, "\"%016llX\",\"%s\",%llu,%.6f,%llu,%.2f\n",
        static_cast<unsigned long long>(row.first), label.c_str(),
        static_cast<unsigned long long>(row.second.executed),
        double(row.second.executed) / double(grand_total),
        static_cast<unsigned long long>(row.second.occurrences),
        double(row.second.host_bytes) / double(row.second.occurrences));
  }
}

bool Processor::AddModule(std::unique_ptr<Module> module) {
  auto global_lock = global_critical_region_.Acquire();
  modules_.push_back(std::move(module));
  return true;
}

void Processor::RemoveModule(const Module* module) {
  auto global_lock = global_critical_region_.Acquire();

  // A title update has the same name as the module it patches.
  auto itr = std::ranges::find_if(
      std::as_const(modules_),
      [module](std::unique_ptr<xe::cpu::Module> const& entry) {
        return entry.get() == module;
      });

  if (itr != modules_.cend()) {
    const std::vector<uint32_t> addressed_functions =
        (*itr)->GetAddressedFunctions();

    modules_.erase(itr);

    for (const uint32_t entry : addressed_functions) {
      RemoveFunctionByAddress(entry);
    }
  }
}

Module* Processor::GetModule(const std::string_view name) {
  auto global_lock = global_critical_region_.Acquire();
  for (const auto& module : modules_) {
    if (module->name() == name) {
      return module.get();
    }
  }
  return nullptr;
}

std::vector<Module*> Processor::GetModules() {
  auto global_lock = global_critical_region_.Acquire();
  std::vector<Module*> clone;
  clone.reserve(modules_.size());
  for (const auto& module : modules_) {
    clone.push_back(module.get());
  }
  return clone;
}

Function* Processor::DefineBuiltin(const std::string_view name,
                                   BuiltinFunction::Handler handler, void* arg0,
                                   void* arg1) {
  uint32_t address = next_builtin_address_;
  next_builtin_address_ += 4;

  Function* function;
  builtin_module_->DeclareFunction(address, &function);
  function->set_end_address(address + 4);
  function->set_name(name);

  auto builtin_function = static_cast<BuiltinFunction*>(function);
  builtin_function->SetupBuiltin(handler, arg0, arg1);

  function->set_status(Symbol::Status::kDeclared);
  return function;
}

Function* Processor::QueryFunction(uint32_t address) {
  auto entry = entry_table_.Get(address);
  if (!entry) {
    return nullptr;
  }
  return entry->function;
}

std::vector<Function*> Processor::FindFunctionsWithAddress(uint32_t address) {
  return entry_table_.FindWithAddress(address);
}

void Processor::RemoveFunctionByAddress(uint32_t address) {
  entry_table_.Delete(address);
}

template <typename Callback>
void Processor::ForEachCodePiece(uint64_t start, uint64_t end,
                                 Callback&& callback) {
  for (uint64_t piece = start; piece < end;) {
    const uint64_t piece_end =
        std::min<uint64_t>((piece & ~uint64_t(0xFFF)) + 0x1000, end);
    const uint64_t key = CodeRangeKey(uint32_t(piece));
    callback(key, key + (piece_end - piece));
    piece = piece_end;
  }
}

template <typename Callback>
void Processor::ForEachCodePiece(const Function* function,
                                 Callback&& callback) {
  const uint64_t start = function->address();
  // The end address is the last instruction's.
  const uint64_t end = std::max<uint64_t>(function->end_address(), start) + 4;
  ForEachCodePiece(start, end, callback);
}

void Processor::InvalidateCodeRange(uint32_t address, uint32_t length) {
  if (!length) {
    return;
  }
  const uint32_t end = address + length - 1;
  auto global_lock = global_critical_region_.Acquire();
  const uint64_t sweep_count = code_sweep_count_.fetch_add(1) + 1;
  RecordSweptCode(address, length);
  auto forget = [this, address, end](Function* function) {
    if (!function) {
      return;
    }
    // The entry is what a call looks up, but the module would hand back the
    // same already defined symbol and never compile the new code.
    if (function->module()) {
      function->module()->ForgetSymbol(function->address());
    }
    // The dynamic call cache keys a function by its entry, which the range
    // misses for a patch inside it or code compiled under another name.
    if (function->address() < address || function->address() > end) {
      backend_->InvalidateDynamicCalls(function->address(),
                                       function->address());
    }
  };
  for (Function* function : entry_table_.DeleteRange(address, end)) {
    forget(function);
  }
  // Dynamic code compiled under another name for the same memory, and the
  // swept keys for compiles in flight to check.
  const bool compiling = !compile_sweep_counts_.empty();
  if (compiling || !dynamic_code_pieces_.empty()) {
    auto sweep_piece = [&](uint64_t key, uint64_t end_key) {
      if (compiling) {
        if (!compile_sweeps_.empty() &&
            compile_sweeps_.back().count == sweep_count &&
            compile_sweeps_.back().end_key == key) {
          compile_sweeps_.back().end_key = end_key;
        } else {
          compile_sweeps_.push_back({sweep_count, key, end_key});
        }
      }
      // No piece crosses a page, which makes the page's first key the lowest
      // that can overlap.
      auto it = dynamic_code_pieces_.lower_bound(key & ~uint64_t(0xFFF));
      while (it != dynamic_code_pieces_.end() && it->first < end_key) {
        if (it->second.end_key <= key) {
          ++it;
          continue;
        }
        const uint32_t function_address = it->second.function_address;
        it = dynamic_code_pieces_.erase(it);
        for (Function* function :
             entry_table_.DeleteRange(function_address, function_address)) {
          forget(function);
        }
      }
    };
    ForEachCodePiece(address, uint64_t(end) + 1, sweep_piece);
  }
  backend_->InvalidateDynamicCalls(address, end);
}

void Processor::RecordDynamicCode(const Function* function) {
  auto global_lock = global_critical_region_.Acquire();
  ForEachCodePiece(function, [&](uint64_t key, uint64_t end_key) {
    // A recompile of the same function reuses its piece rather than piling up.
    auto [first, last] = dynamic_code_pieces_.equal_range(key);
    auto same = std::find_if(first, last, [function](const auto& existing) {
      return existing.second.function_address == function->address();
    });
    if (same != last) {
      same->second.end_key = std::max(same->second.end_key, end_key);
    } else {
      dynamic_code_pieces_.emplace(
          key, DynamicCodePiece{end_key, function->address()});
    }
  });
}

uint64_t Processor::BeginCompile() {
  const uint64_t sweep_count = code_sweep_count_.load();
  compile_sweep_counts_.insert(sweep_count);
  return sweep_count;
}

void Processor::EndCompile(uint64_t sweep_count) {
  compile_sweep_counts_.erase(compile_sweep_counts_.find(sweep_count));
  // Only sweeps after the oldest compile still in flight began can matter.
  const uint64_t oldest = compile_sweep_counts_.empty()
                              ? UINT64_MAX
                              : *compile_sweep_counts_.begin();
  while (!compile_sweeps_.empty() && compile_sweeps_.front().count <= oldest) {
    compile_sweeps_.pop_front();
  }
}

bool Processor::SweptSince(const Function* function, uint64_t sweep_count) {
  if (compile_sweeps_.empty() || compile_sweeps_.back().count <= sweep_count) {
    return false;
  }
  bool swept = false;
  ForEachCodePiece(function, [&](uint64_t key, uint64_t end_key) {
    // The log is in count order, so only its tail is newer.
    for (auto sweep = compile_sweeps_.rbegin();
         !swept && sweep != compile_sweeps_.rend() &&
         sweep->count > sweep_count;
         ++sweep) {
      swept = sweep->start_key < end_key && key < sweep->end_key;
    }
  });
  return swept;
}

void Processor::RecordSweptCode(uint32_t address, uint32_t length) {
  if (!dynamic_code_enabled_.load()) {
    // Only dynamic code is bounded this way, and nothing prunes the map.
    return;
  }
  // Spans are keyed by physical address, so a range whose pages aren't
  // physically contiguous would claim someone else's memory.
  const uint64_t span_start = CodeRangeKey(address);
  const uint64_t span_end = span_start + length;
  if (CodeRangeKey(address + length - 1) != span_end - 1) {
    return;
  }
  // Spans stay exact rather than merging, since back-to-back blocks must not
  // bound each other. A sweep replaces any span it overlaps, except a single
  // instruction patched past a span's start, which keeps the block's end.
  auto range = swept_code_ranges_.lower_bound(span_start);
  if (range != swept_code_ranges_.begin()) {
    auto previous = range;
    --previous;
    if (previous->second > span_start) {
      if (length == 4 && previous->second >= span_end) {
        return;
      }
      range = previous;
    }
  }
  while (range != swept_code_ranges_.end() && range->first < span_end) {
    range = swept_code_ranges_.erase(range);
  }
  swept_code_ranges_.emplace(span_start, span_end);
}

uint64_t Processor::CodeRangeKey(uint32_t address) {
  const uint32_t kernel_address = memory_->UserModeKernelAddress(address);
  auto heap = memory_->LookupHeap(kernel_address);
  if (heap && heap->heap_type() == HeapType::kGuestPhysical) {
    const uint32_t physical_address =
        memory_->GetPhysicalAddress(kernel_address);
    if (physical_address != UINT32_MAX) {
      return kPhysicalCodeKey | physical_address;
    }
  }
  return kernel_address;
}

uint32_t Processor::SweptCodeEnd(uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  const uint64_t key = CodeRangeKey(address);
  auto range = swept_code_ranges_.upper_bound(key);
  if (range == swept_code_ranges_.begin()) {
    return 0;
  }
  --range;
  if (key >= range->second) {
    return 0;
  }
  // The range may be keyed by another address for the same memory, so convert
  // back to this one.
  return address + uint32_t(range->second - key);
}

Function* Processor::ResolveCallTarget(ppc::PPCContext* context,
                                       uint32_t* address) {
  Function* function = ResolveFunction(*address);
  while (!function || !function->is_guest()) {
    const CodeFaultHook hook = code_fault_hook();
    const uint32_t resume_address = hook ? hook(context, *address) : 0;
    if (!resume_address) {
      return nullptr;
    }
    *address = resume_address;
    function = ResolveFunction(*address);
  }
  return function;
}

Function* Processor::ResolveFunction(uint32_t address) {
  Entry* entry;
  Entry::Status status = entry_table_.GetOrCreate(address, &entry);
  if (status == Entry::STATUS_NEW) {
    // Needs to be generated. We have the 'lock' on it and must do so now.
    Module* module = LookupModule(address);
    // A sweep of dynamic code while it compiles means the compile may have
    // read the old code, so it runs again. Code of a loaded module doesn't,
    // since its loader set up some of its symbols and a new symbol would lack
    // that.
    const bool dynamic = IsDynamicCode(module);
    uint64_t sweep_count = 0;
    if (dynamic) {
      auto global_lock = global_critical_region_.Acquire();
      sweep_count = BeginCompile();
    }
    constexpr uint32_t kMaxCompileAttempts = 4;
    for (uint32_t attempt = 1;; ++attempt) {
      // Grab symbol declaration.
      auto function = module ? LookupFunction(module, address) : nullptr;
      const bool defined = function && DemandFunction(function);

      if (defined) {
        // only add it to the list of resolved functions if resolving succeeded
        auto xexmod = dynamic_cast<XexModule*>(module);
        if (xexmod) {
          auto addr_flags = xexmod->GetInstructionAddressFlags(address);
          if (addr_flags) {
            InfoCacheFlags bits{};
            bits.was_resolved = 1;
            AtomicSetInfoCacheFlags(addr_flags, bits);
          }
        }
      }

      // A sweep must not land between the check and the publish, nor find
      // dynamic code ready before its pieces are recorded.
      auto global_lock = global_critical_region_.Acquire();
      bool swept = false;
      if (dynamic) {
        swept = defined && SweptSince(function, sweep_count);
        EndCompile(sweep_count);
      }
      if (!defined) {
        entry_table_.MarkFailed(entry);
        return nullptr;
      }
      if (swept) {
        // The module would hand back the same already defined symbol.
        module->ForgetSymbol(address);
        if (attempt < kMaxCompileAttempts) {
          sweep_count = BeginCompile();
          continue;
        }
        // Code the guest keeps rewriting runs once for the callers waiting on
        // it, and the next call compiles it again. The count tells those that
        // cache it to drop it.
        entry_table_.MarkReady(entry, function, function->end_address());
        entry_table_.Delete(address);
        code_sweep_count_.fetch_add(1);
        return function;
      }
      entry_table_.MarkReady(entry, function, function->end_address());
      if (dynamic) {
        RecordDynamicCode(function);
      }
      return function;
    }
  }
  if (status == Entry::STATUS_READY) {
    // Ready to use.
    return entry->function;
  } else {
    // Failed or bad state.
    return nullptr;
  }
}

size_t Processor::ResolveFunctionsInParallel(
    std::vector<uint32_t> addresses,
    const std::function<void(Function*, std::vector<uint32_t>&)>& expand) {
  if (addresses.empty()) {
    return 0;
  }
  std::mutex lock;
  std::condition_variable cv;
  std::unordered_set<uint32_t> queued(addresses.begin(), addresses.end());
  size_t active = 0;
  size_t resolved = 0;
  auto work = [&]() {
    std::vector<uint32_t> found;
    std::unique_lock<std::mutex> guard(lock);
    while (true) {
      cv.wait(guard, [&]() { return !addresses.empty() || !active; });
      if (addresses.empty()) {
        return;
      }
      uint32_t address = addresses.back();
      addresses.pop_back();
      ++active;
      guard.unlock();
      Function* function = ResolveFunction(address);
      found.clear();
      if (function && expand) {
        expand(function, found);
      }
      guard.lock();
      resolved += function ? 1 : 0;
      for (uint32_t next : found) {
        if (queued.insert(next).second) {
          addresses.push_back(next);
        }
      }
      --active;
      cv.notify_all();
    }
  };
  size_t count = std::clamp<size_t>(xe::threading::logical_processor_count(), 1,
                                    addresses.size());
  std::vector<std::unique_ptr<xe::threading::Thread>> threads;
  xe::threading::Thread::CreationParameters params;
  // Optimizer passes recurse, so match a guest fiber's stack.
  params.stack_size = 16_MiB;
  for (size_t i = 0; i < count; ++i) {
    auto thread = xe::threading::Thread::Create(params, [&work, i]() {
      std::string name = "Precompile " + std::to_string(i);
      xe::threading::set_name(name);
      Profiler::ThreadEnter(name.c_str());
      work();
      Profiler::ThreadExit();
    });
    if (thread) {
      threads.push_back(std::move(thread));
    }
  }
  if (threads.empty()) {
    work();
  }
  for (auto& thread : threads) {
    xe::threading::Wait(thread.get());
  }
  return resolved;
}

Module* Processor::LookupModule(uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  // TODO(benvanik): sort by code address (if contiguous) so can bsearch.
  // TODO(benvanik): cache last module low/high, as likely to be in there.
  for (const auto& module : modules_) {
    if (module->ContainsAddress(address)) {
      return module.get();
    }
  }
  if (dynamic_code_module_ && dynamic_code_module_->ContainsAddress(address)) {
    return dynamic_code_module_.get();
  }
  return nullptr;
}

void Processor::BeginVscrSatCensus() {
  std::lock_guard<std::mutex> lock(vscr_sat_mutex_);
  vscr_sat_reader_ = false;
  vscr_sat_census_ = true;
  vscr_sat_observable_.store(true);
}

void Processor::NoteVscrSatReader(std::string_view module_name) {
  std::lock_guard<std::mutex> lock(vscr_sat_mutex_);
  if (vscr_sat_census_) {
    vscr_sat_reader_ = true;
    return;
  }
  if (!vscr_sat_observable_.exchange(true)) {
    XELOGW("{} reads VSCR[SAT], which code compiled before it doesn't set",
           module_name);
  }
}

void Processor::FinishVscrSatCensus(bool executable_reads) {
  std::lock_guard<std::mutex> lock(vscr_sat_mutex_);
  const bool observable =
      executable_reads || vscr_sat_reader_ || dynamic_code_enabled();
  vscr_sat_observable_.store(observable);
  vscr_sat_census_ = false;
  XELOGI("VSCR[SAT] {}", observable ? "is read, saturation sets it"
                                    : "is never read, saturation skips it");
}

void Processor::EnableDynamicCode() {
  auto global_lock = global_critical_region_.Acquire();
  if (!dynamic_code_module_) {
    dynamic_code_module_ = std::make_unique<DynamicCodeModule>(this);
  }
  dynamic_code_enabled_.store(true, std::memory_order_relaxed);
  // Code the guest writes can read it.
  std::lock_guard<std::mutex> lock(vscr_sat_mutex_);
  if (!vscr_sat_observable_.exchange(true)) {
    XELOGW(
        "Dynamic code may read VSCR[SAT], which code compiled before it "
        "doesn't set");
  }
}

Function* Processor::LookupFunction(uint32_t address) {
  // TODO(benvanik): fast reject invalid addresses/log errors.

  // Find the module that contains the address.
  Module* code_module = LookupModule(address);

  if (!code_module) {
    // No module found that could contain the address.
    return nullptr;
  }

  return LookupFunction(code_module, address);
}

Function* Processor::LookupFunction(Module* module, uint32_t address) {
  // Atomic create/lookup symbol in module.
  // If we get back the NEW flag we must declare it now.
  Function* function = nullptr;
  auto symbol_status = module->DeclareFunction(address, &function);
  if (symbol_status == Symbol::Status::kNew) {
    // Symbol is undeclared, so declare now.
    assert_true(function->is_guest());
    if (!frontend_->DeclareFunction(static_cast<GuestFunction*>(function))) {
      function->set_status(Symbol::Status::kFailed);
      return nullptr;
    }
    function->set_status(Symbol::Status::kDeclared);
  }
  return function;
}

bool Processor::DemandFunction(Function* function) {
  // Lock function for generation. If it's already being generated
  // by another thread this will block and return DECLARED.
  auto module = function->module();
  auto symbol_status = module->DefineFunction(function);
  if (symbol_status == Symbol::Status::kNew) {
    // Symbol is undefined, so define now.
    assert_true(function->is_guest());
    if (IsDynamicCode(module)) {
      static_cast<DynamicCodeModule*>(module)->BoundFunction(
          static_cast<GuestFunction*>(function));
    }
    if (!frontend_->DefineFunction(static_cast<GuestFunction*>(function),
                                   debug_info_flags_)) {
      function->set_status(Symbol::Status::kFailed);
      return false;
    }

    function->set_status(Symbol::Status::kDefined);
    symbol_status = function->status();
  }

  if (symbol_status == Symbol::Status::kFailed) {
    // Symbol likely failed.
    return false;
  }

  return true;
}

bool Processor::Execute(ThreadState* thread_state, uint32_t address) {
  SCOPE_profile_cpu_f("cpu");

  // Attempt to get the function.
  auto function = ResolveFunction(address);
  if (!function) {
    // Symbol not found in any module.
    XELOGCPU("Execute({:08X}): failed to find function", address);
    return false;
  }

  auto context = thread_state->context();

  // Pad out stack a bit, as some games seem to overwrite the caller by about
  // 16 to 32b.
  context->r[1] -= 64 + 112;

  // This could be set to anything to give us a unique identifier to track
  // re-entrancy/etc.
  uint64_t previous_lr = context->lr;
  context->lr = 0xBCBCBCBC;

  // Execute the function.
  auto result = function->Call(thread_state, uint32_t(context->lr));

  context->lr = previous_lr;
  context->r[1] += 64 + 112;

  return result;
}

bool Processor::ExecuteRaw(ThreadState* thread_state, uint32_t address) {
  SCOPE_profile_cpu_f("cpu");

  // Attempt to get the function.
  auto function = ResolveFunction(address);
  if (!function) {
    // Symbol not found in any module.
    XELOGCPU("Execute({:08X}): failed to find function", address);
    return false;
  }

  return function->Call(thread_state, 0xBCBCBCBC);
}

uint64_t Processor::Execute(ThreadState* thread_state, uint32_t address,
                            uint64_t args[], size_t arg_count) {
  SCOPE_profile_cpu_f("cpu");

  auto context = thread_state->context();
  for (size_t i = 0; i < std::min(arg_count, static_cast<size_t>(8)); ++i) {
    context->r[3 + i] = args[i];
  }

  if (arg_count > 7) {
    // Rest of the arguments go on the stack.
    // FIXME: This assumes arguments are 32 bits!
    auto stack_arg_base =
        memory()->TranslateVirtual((uint32_t)context->r[1] + 0x54 - (64 + 112));
    for (size_t i = 0; i < arg_count - 8; i++) {
      xe::store_and_swap<uint32_t>(stack_arg_base + (i * 8),
                                   (uint32_t)args[i + 8]);
    }
  }

  if (!Execute(thread_state, address)) {
    return 0xDEADBABE;
  }
  return context->r[3];
}

// Address space only, committed as functions reserve offsets. 8 bytes per
// guest instruction, so this covers 32M translated instructions.
static constexpr size_t kTraceCountsArenaSize = 256_MiB;

bool Processor::CommitTraceCountsLocked(size_t required) {
  size_t target = xe::round_up(required, xe::memory::allocation_granularity());
  if (target <= trace_counts_committed_) {
    return true;
  }
  size_t length = target - trace_counts_committed_;
  for (auto& arena : trace_counts_arenas_) {
    if (!xe::memory::AllocFixed(arena.base + trace_counts_committed_, length,
                                xe::memory::AllocationType::kCommit,
                                xe::memory::PageAccess::kReadWrite)) {
      return false;
    }
  }
  trace_counts_committed_ = target;
  return true;
}

void Processor::FoldTraceCountsLocked(uint8_t* arena) {
  for (auto& region : trace_counts_regions_) {
    auto counts = reinterpret_cast<uint64_t*>(arena + region.offset);
    for (size_t i = 0; i < region.count; ++i) {
      if (counts[i]) {
        region.retired[i] += counts[i];
        counts[i] = 0;
      }
    }
  }
}

uint8_t* Processor::ReserveTraceCountsArenaLocked(uint32_t thread_id) {
  auto base = reinterpret_cast<uint8_t*>(xe::memory::AllocFixed(
      nullptr, kTraceCountsArenaSize, xe::memory::AllocationType::kReserve,
      xe::memory::PageAccess::kNoAccess));
  if (!base) {
    return nullptr;
  }
  // Catch up to what the existing arenas already have committed.
  if (trace_counts_committed_ &&
      !xe::memory::AllocFixed(base, trace_counts_committed_,
                              xe::memory::AllocationType::kCommit,
                              xe::memory::PageAccess::kReadWrite)) {
    xe::memory::DeallocFixed(base, 0, xe::memory::DeallocationType::kRelease);
    return nullptr;
  }
  trace_counts_arenas_.push_back({base, thread_id});
  return base;
}

void Processor::SetTraceCountsArenaThreadLocked(uint8_t* arena,
                                                uint32_t thread_id) {
  for (auto& entry : trace_counts_arenas_) {
    if (entry.base == arena) {
      entry.thread_id = thread_id;
      return;
    }
  }
}

// Threads that cannot get a private arena share the fallback. The emitted
// counters do not null check, so no counting code may be generated until the
// fallback exists, and if it cannot be had coverage stays off for good.
bool Processor::EnsureTraceCountsFallbackLocked() {
  if (trace_counts_fallback_) {
    return true;
  }
  if (trace_counts_failed_) {
    return false;
  }
  trace_counts_fallback_ = ReserveTraceCountsArenaLocked(0);
  if (!trace_counts_fallback_) {
    trace_counts_failed_ = true;
    XELOGW("Unable to reserve an instruction coverage arena, coverage is off");
    return false;
  }
  return true;
}

size_t Processor::AllocateTraceCountsOffset(uint32_t start_address,
                                            uint32_t instruction_count) {
  size_t byte_count = size_t(instruction_count) * 8;
  std::lock_guard<std::mutex> lock(trace_counts_mutex_);
  if (!EnsureTraceCountsFallbackLocked()) {
    return GuestFunction::kInvalidCoverageOffset;
  }
  size_t offset = trace_counts_next_offset_;
  if (byte_count > kTraceCountsArenaSize - offset) {
    return GuestFunction::kInvalidCoverageOffset;
  }
  if (!CommitTraceCountsLocked(offset + byte_count)) {
    return GuestFunction::kInvalidCoverageOffset;
  }
  trace_counts_next_offset_ = offset + byte_count;
  TraceCountsRegion region;
  region.start_address = start_address;
  region.offset = offset;
  region.count = instruction_count;
  region.retired = std::make_unique<uint64_t[]>(instruction_count);
  trace_counts_regions_.push_back(std::move(region));
  return offset;
}

uint8_t* Processor::AcquireTraceCounts(uint32_t thread_id) {
  std::lock_guard<std::mutex> lock(trace_counts_mutex_);
  // Every thread gets an arena even with coverage off, because it can come on
  // at a later title launch and the emitted counters do not null check. Until
  // then the shared one will do, nothing is counting into it.
  if (!EnsureTraceCountsFallbackLocked()) {
    return nullptr;
  }
  if (!trace_counts_enabled_) {
    return trace_counts_fallback_;
  }
  if (!trace_counts_free_.empty()) {
    uint8_t* arena = trace_counts_free_.back();
    trace_counts_free_.pop_back();
    SetTraceCountsArenaThreadLocked(arena, thread_id);
    return arena;
  }
  uint8_t* arena = ReserveTraceCountsArenaLocked(thread_id);
  if (!arena) {
    XELOGW("Sharing the fallback instruction coverage arena, counts will race");
    return trace_counts_fallback_;
  }
  return arena;
}

void Processor::RecordSequenceSamples(
    uint32_t start_address, std::vector<backend::SequenceSample> samples) {
  std::lock_guard<std::mutex> lock(trace_counts_mutex_);
  // Searched from the back: the translator reserves a function's region
  // immediately before handing it to the backend, so it is normally the last.
  for (auto it = trace_counts_regions_.rbegin();
       it != trace_counts_regions_.rend(); ++it) {
    if (it->start_address == start_address) {
      it->samples = std::move(samples);
      return;
    }
  }
}

void Processor::ResetTraceCounts() {
  std::lock_guard<std::mutex> lock(trace_counts_mutex_);
  for (auto& arena : trace_counts_arenas_) {
    std::memset(arena.base, 0, trace_counts_committed_);
  }
  for (auto& region : trace_counts_regions_) {
    std::memset(region.retired.get(), 0, region.count * sizeof(uint64_t));
  }
}

void Processor::ReleaseTraceCounts(uint8_t* arena) {
  if (!arena) {
    return;
  }
  std::lock_guard<std::mutex> lock(trace_counts_mutex_);
  if (arena == trace_counts_fallback_) {
    // Still shared with other threads, folded at teardown instead.
    return;
  }
  FoldTraceCountsLocked(arena);
  SetTraceCountsArenaThreadLocked(arena, 0);
  trace_counts_free_.push_back(arena);
}

// These run a real lwarx/stwcx. loop, so they cancel any guest thread's
// reservation on the same granule. Each returns the pre-update value.
// context must be the calling thread's, it holds the reservation.
uint32_t Processor::GuestAtomicIncrement32(ppc::PPCContext* context,
                                           uint32_t guest_address) {
  uint32_t result;
  do {
    result = backend()->ReservedLoad32(context, guest_address);
  } while (!backend()->ReservedStore32(context, guest_address, result + 1));
  return result;
}
uint32_t Processor::GuestAtomicDecrement32(ppc::PPCContext* context,
                                           uint32_t guest_address) {
  uint32_t result;
  do {
    result = backend()->ReservedLoad32(context, guest_address);
  } while (!backend()->ReservedStore32(context, guest_address, result - 1));
  return result;
}

uint32_t Processor::GuestAtomicOr32(ppc::PPCContext* context,
                                    uint32_t guest_address, uint32_t mask) {
  uint32_t result;
  do {
    result = backend()->ReservedLoad32(context, guest_address);
  } while (!backend()->ReservedStore32(context, guest_address, result | mask));
  return result;
}
uint32_t Processor::GuestAtomicXor32(ppc::PPCContext* context,
                                     uint32_t guest_address, uint32_t mask) {
  uint32_t result;
  do {
    result = backend()->ReservedLoad32(context, guest_address);
  } while (!backend()->ReservedStore32(context, guest_address, result ^ mask));
  return result;
}
uint32_t Processor::GuestAtomicAnd32(ppc::PPCContext* context,
                                     uint32_t guest_address, uint32_t mask) {
  uint32_t result;
  do {
    result = backend()->ReservedLoad32(context, guest_address);
  } while (!backend()->ReservedStore32(context, guest_address, result & mask));
  return result;
}

// Does not retry. A lost reservation reports a failed exchange, which is what
// a guest compare-exchange loop sees.
bool Processor::GuestAtomicCAS32(ppc::PPCContext* context, uint32_t old_value,
                                 uint32_t new_value, uint32_t guest_address) {
  if (backend()->ReservedLoad32(context, guest_address) != old_value) {
    return false;
  }
  return backend()->ReservedStore32(context, guest_address, new_value);
}
}  // namespace cpu
}  // namespace xe
