/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_MODULE_H_
#define XENIA_CPU_MODULE_H_

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/base/mutex.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/symbol.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {

class Processor;

struct InfoCacheFlags {
  uint32_t was_resolved : 1;  // has this address ever been called/requested
                              // via resolvefunction?
  uint32_t accessed_mmio : 1;
  uint32_t is_syscall_func : 1;
  uint32_t is_return_site : 1;  // address can be reached from another function
                                // by returning
  uint32_t reserved : 28;
};
static_assert(sizeof(InfoCacheFlags) == 4,
              "InfoCacheFlags size should be equal to sizeof ppc instruction.");

// Each slot is one 32-bit word (XexModule's shared info-cache mmap, or
// RawModule's vector) written by several threads. Set bits atomically so
// concurrent writes don't lose updates.
inline void AtomicSetInfoCacheFlags(InfoCacheFlags* slot, InfoCacheFlags bits) {
  uint32_t mask;
  std::memcpy(&mask, &bits, sizeof(mask));
  std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(slot))
      .fetch_or(mask, std::memory_order_relaxed);
}

class Module {
 public:
  explicit Module(Processor* processor);
  virtual ~Module();

  Memory* memory() const { return memory_; }

  virtual const std::string& name() const = 0;
  virtual bool is_executable() const = 0;

  virtual bool ContainsAddress(uint32_t address);

  // Per-instruction flag slot for |guest_address|, or null when the module
  // keeps no instruction metadata for it. XexModule backs this with its
  // shared info cache; RawModule with a plain vector so the longjmp repair
  // path works under the test harness too.
  virtual InfoCacheFlags* GetInstructionAddressFlags(uint32_t guest_address) {
    return nullptr;
  }

  Symbol* LookupSymbol(uint32_t address, bool wait = true);
  virtual Symbol::Status DeclareFunction(uint32_t address,
                                         Function** out_function);
  virtual Symbol::Status DeclareVariable(uint32_t address, Symbol** out_symbol);

  Symbol::Status DefineFunction(Function* symbol);
  Symbol::Status DefineVariable(Symbol* symbol);

  const std::vector<uint32_t> GetAddressedFunctions();
  void ForEachFunction(std::function<void(Function*)> callback);
  void ForEachSymbol(size_t start_index, size_t end_index,
                     std::function<void(Symbol*)> callback);
  size_t QuerySymbolCount();

  bool ReadMap(const char* file_name);

  virtual void Precompile() {}

 protected:
  virtual std::unique_ptr<Function> CreateFunction(uint32_t address) = 0;

  Processor* processor_ = nullptr;
  Memory* memory_ = nullptr;

 private:
  Symbol::Status DeclareSymbol(Symbol::Type type, uint32_t address,
                               Symbol** out_symbol);
  Symbol::Status DefineSymbol(Symbol* symbol);

  xe::global_critical_region global_critical_region_;
  // TODO(benvanik): replace with a better data structure.
  std::unordered_map<uint32_t, Symbol*> map_;
  std::vector<std::unique_ptr<Symbol>> list_;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_MODULE_H_
