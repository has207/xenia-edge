/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <cstring>

#include "xenia/base/debugging.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_rtl.h"
#include "xenia/kernel/xthread.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

void DbgBreakPoint_entry() { xe::debugging::Break(); }
DECLARE_XBOXKRNL_EXPORT2(DbgBreakPoint, kDebug, kStub, kImportant);

// https://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx
typedef struct {
  xe::be<uint32_t> type;
  xe::be<uint32_t> name_ptr;
  xe::be<uint32_t> thread_id;
  xe::be<uint32_t> flags;
} X_THREADNAME_INFO;
static_assert_size(X_THREADNAME_INFO, 0x10);

void HandleSetThreadName(pointer_t<X_EXCEPTION_RECORD> record) {
  // SetThreadName. FFS.
  // https://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx

  // TODO(benvanik): check record->number_parameters to make sure it's a
  // correct size.
  auto thread_info =
      reinterpret_cast<X_THREADNAME_INFO*>(&record->exception_information[0]);

  assert_true(thread_info->type == 0x1000);

  if (!thread_info->name_ptr) {
    XELOGD("SetThreadName called with null name_ptr");
    return;
  }

  // 4D5307D6 (and its demo) has a bug where it ends up passing freed memory for
  // the name, so at the point of SetThreadName it's filled with junk.

  // TODO(gibbed): cvar for thread name encoding for conversion, some games use
  // SJIS and there's no way to automatically know this.
  auto name = std::string(
      kernel_memory()->TranslateVirtual<const char*>(thread_info->name_ptr));
  std::ranges::replace_if(name, [](auto c) { return c < 32 || c > 127; }, '?');

  object_ref<XThread> thread;
  if (thread_info->thread_id == -1) {
    // Current thread.
    thread = retain_object(XThread::GetCurrentThread());
  } else {
    // Lookup thread by ID.
    thread = kernel_state()->GetThreadByID(thread_info->thread_id);
  }

  if (thread) {
    XELOGD("SetThreadName({}, {})", thread->thread_id(), name);
    thread->set_name(name);
  }

  // TODO(benvanik): unwinding required here?
}

typedef struct {
  xe::be<int32_t> mdisp;
  xe::be<int32_t> pdisp;
  xe::be<int32_t> vdisp;
} x_PMD;

typedef struct {
  xe::be<uint32_t> properties;
  xe::be<uint32_t> type_ptr;
  x_PMD this_displacement;
  xe::be<int32_t> size_or_offset;
  xe::be<uint32_t> copy_function_ptr;
} x_s__CatchableType;

typedef struct {
  xe::be<int32_t> number_catchable_types;
  xe::be<uint32_t> catchable_type_ptrs[1];
} x_s__CatchableTypeArray;

typedef struct {
  xe::be<uint32_t> attributes;
  xe::be<uint32_t> unwind_ptr;
  xe::be<uint32_t> forward_compat_ptr;
  xe::be<uint32_t> catchable_type_array_ptr;
} x_s__ThrowInfo;

// MSVC RTTI type descriptor, the mangled name follows the two pointers.
typedef struct {
  xe::be<uint32_t> vftable_ptr;
  xe::be<uint32_t> spare_ptr;
  char name[1];
} x_TypeDescriptor;

// The C++ EH tables a __CxxFrameHandler frame points at.
typedef struct {
  xe::be<uint32_t> magic_number;
  xe::be<int32_t> max_state;
  xe::be<uint32_t> unwind_map_ptr;
  xe::be<int32_t> try_block_count;
  xe::be<uint32_t> try_block_map_ptr;
  xe::be<int32_t> ip_map_entry_count;
  xe::be<uint32_t> ip_to_state_map_ptr;
} x_FuncInfo;

typedef struct {
  xe::be<int32_t> try_low;
  xe::be<int32_t> try_high;
  xe::be<int32_t> catch_high;
  xe::be<int32_t> catch_count;
  xe::be<uint32_t> handler_array_ptr;
} x_TryBlockMapEntry;

typedef struct {
  xe::be<uint32_t> adjectives;
  xe::be<uint32_t> type_ptr;
  xe::be<int32_t> catch_object_displacement;
  xe::be<uint32_t> handler_address;
} x_HandlerType;

typedef struct {
  xe::be<uint32_t> ip;
  xe::be<int32_t> state;
} x_IpToStateMapEntry;

// True when every page of [address, address + size) is mapped and readable.
static bool IsGuestRangeReadable(uint32_t address, uint32_t size) {
  if (!address || !size || address > 0xFFFFFFFFu - (size - 1)) {
    return false;
  }
  auto* memory = kernel_memory();
  const uint32_t last_page = (address + size - 1) & ~uint32_t(0xFFF);
  for (uint32_t page = address & ~uint32_t(0xFFF);; page += 0x1000) {
    auto* heap = memory->LookupHeap(page);
    uint32_t protect = 0;
    if (!heap || !heap->QueryProtect(page, &protect) ||
        !(protect & kMemoryProtectRead)) {
      return false;
    }
    if (page == last_page) {
      return true;
    }
  }
}

static uint32_t ReadGuestWord(uint32_t address) {
  return *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(address);
}

// Reads a NUL terminated guest string, stopping at the first unreadable page.
static std::string ReadGuestString(uint32_t address, uint32_t max_length) {
  std::string value;
  while (value.size() < max_length) {
    uint32_t chunk = 0x1000 - (address & 0xFFF);
    const uint32_t remaining = max_length - static_cast<uint32_t>(value.size());
    if (chunk > remaining) {
      chunk = remaining;
    }
    if (!IsGuestRangeReadable(address, chunk)) {
      break;
    }
    auto* host = kernel_memory()->TranslateVirtual<const char*>(address);
    uint32_t length = 0;
    while (length < chunk && host[length]) {
      ++length;
    }
    value.append(host, length);
    if (length < chunk) {
      break;
    }
    address += chunk;
  }
  std::ranges::replace_if(value, [](auto c) { return c < 32 || c > 127; }, '?');
  return value;
}

// Reads the mangled name out of an RTTI type descriptor.
static std::string ReadGuestTypeName(uint32_t descriptor_ptr) {
  if (!IsGuestRangeReadable(descriptor_ptr, sizeof(x_TypeDescriptor))) {
    return {};
  }
  return ReadGuestString(descriptor_ptr + offsetof(x_TypeDescriptor, name),
                         256);
}

// Describes the try blocks and catch types a frame declares.
// Returns nothing unless the handler data is a FuncInfo, which rejects a plain
// __C_specific_handler scope table.
static std::string DescribeGuestFuncInfo(uint32_t func_info_ptr) {
  if (!IsGuestRangeReadable(func_info_ptr, sizeof(x_FuncInfo))) {
    return {};
  }
  auto* info = kernel_memory()->TranslateVirtual<x_FuncInfo*>(func_info_ptr);
  const uint32_t magic = info->magic_number;
  if (magic < 0x19930520 || magic > 0x19930522) {
    return {};
  }
  int32_t try_block_count = info->try_block_count;
  if (try_block_count > 16) {
    try_block_count = 16;
  }
  std::string text = fmt::format(
      "\n      func_info magic=0x{:08X} max_state={} try_blocks={}", magic,
      int32_t(info->max_state), int32_t(info->try_block_count));
  const uint32_t try_block_map_ptr = info->try_block_map_ptr;
  for (int32_t i = 0; i < try_block_count; ++i) {
    const uint32_t try_ptr =
        try_block_map_ptr + uint32_t(i) * uint32_t(sizeof(x_TryBlockMapEntry));
    if (!IsGuestRangeReadable(try_ptr, sizeof(x_TryBlockMapEntry))) {
      break;
    }
    auto* try_block =
        kernel_memory()->TranslateVirtual<x_TryBlockMapEntry*>(try_ptr);
    int32_t catch_count = try_block->catch_count;
    text.append(fmt::format(
        "\n      try[{}] low={} high={} catch_high={} catches={}", i,
        int32_t(try_block->try_low), int32_t(try_block->try_high),
        int32_t(try_block->catch_high), catch_count));
    if (catch_count > 16) {
      catch_count = 16;
    }
    const uint32_t handler_array_ptr = try_block->handler_array_ptr;
    for (int32_t j = 0; j < catch_count; ++j) {
      const uint32_t handler_ptr =
          handler_array_ptr + uint32_t(j) * uint32_t(sizeof(x_HandlerType));
      if (!IsGuestRangeReadable(handler_ptr, sizeof(x_HandlerType))) {
        break;
      }
      auto* handler =
          kernel_memory()->TranslateVirtual<x_HandlerType*>(handler_ptr);
      const uint32_t type_ptr = handler->type_ptr;
      text.append(fmt::format(
          "\n        catch {} adjectives=0x{:X} handler=0x{:08X}",
          type_ptr ? ReadGuestTypeName(type_ptr) : "...",
          uint32_t(handler->adjectives), uint32_t(handler->handler_address)));
    }
  }
  return text;
}

// The exception directory of the image at base: the .pdata entries, as the
// kernel's RtlLookupFunctionEntry finds them through XexPcToFileHeader and
// RtlImageDirectoryEntryToData. Images such as the CLR's native images keep it
// in another section than .pdata. The PE headers are little-endian.
static const xe::be<uint32_t>* GetExceptionDirectory(uint32_t base,
                                                     uint32_t* out_size) {
  constexpr uint32_t kExceptionDirectory = 3;
  if (!IsGuestRangeReadable(base, 0x40)) {
    return nullptr;
  }
  const auto* image = kernel_memory()->TranslateVirtual<const uint8_t*>(base);
  if (xe::load<uint16_t>(image) != 0x5A4D) {  // MZ
    return nullptr;
  }
  const uint32_t nt = base + xe::load<uint32_t>(image + 0x3C);
  // Signature, file header, then the PE32 optional header, whose data
  // directories follow its count at 0x5C.
  const uint32_t optional = nt + 0x18;
  if (!IsGuestRangeReadable(nt, 0x18 + 0x60 + (kExceptionDirectory + 1) * 8)) {
    return nullptr;
  }
  const auto* header = kernel_memory()->TranslateVirtual<const uint8_t*>(nt);
  if (xe::load<uint32_t>(header) != 0x4550 ||  // PE
      xe::load<uint16_t>(header + 0x18) != 0x10B ||
      xe::load<uint32_t>(header + 0x18 + 0x5C) <= kExceptionDirectory) {
    return nullptr;
  }
  const auto* directory = kernel_memory()->TranslateVirtual<const uint8_t*>(
      optional + 0x60 + kExceptionDirectory * 8);
  const uint32_t address = xe::load<uint32_t>(directory);
  const uint32_t size = xe::load<uint32_t>(directory + 4);
  if (!address || !size || !IsGuestRangeReadable(base + address, size)) {
    return nullptr;
  }
  *out_size = size;
  return kernel_memory()->TranslateVirtual<const xe::be<uint32_t>*>(base +
                                                                    address);
}

// Locates the .pdata of the module holding address: its exception directory,
// else its .pdata section with the zero padding trimmed. out_base, when given,
// gets the entries' guest address. The kernel also searches a second table
// the loader entry points to, at 0x44; xenia leaves that field 0.
static const xe::be<uint32_t>* GetGuestPdata(uint32_t address,
                                             uint32_t* out_count,
                                             uint32_t* out_base = nullptr) {
  auto* xex = dynamic_cast<xe::cpu::XexModule*>(
      kernel_state()->processor()->LookupModule(address));
  if (!xex) {
    return nullptr;
  }
  uint32_t size = 0;
  if (const auto* entries = GetExceptionDirectory(xex->base_address(), &size)) {
    *out_count = size / 8;
    if (out_base) {
      *out_base = kernel_memory()->HostToGuestVirtual(entries);
    }
    return entries;
  }
  auto* section = xex->GetPESection(".pdata");
  if (!section || !IsGuestRangeReadable(section->address, section->raw_size)) {
    return nullptr;
  }
  auto* entries = kernel_memory()->TranslateVirtual<const xe::be<uint32_t>*>(
      section->address);
  uint32_t count = 0;
  while (count < section->raw_size / 8 && uint32_t(entries[count * 2]) != 0) {
    ++count;
  }
  *out_count = count;
  if (out_base) {
    *out_base = section->address;
  }
  return entries;
}

// Finds the .pdata entry covering address. Entries are sorted by start.
// out_entry, when given, gets the guest address of the entry itself.
static bool LookupGuestRuntimeFunction(uint32_t address, uint32_t* out_start,
                                       uint32_t* out_packed,
                                       uint32_t* out_entry = nullptr) {
  uint32_t count = 0;
  uint32_t base = 0;
  const auto* entries = GetGuestPdata(address, &count, &base);
  if (!entries || !count) {
    return false;
  }
  uint32_t low = 0;
  uint32_t high = count;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2;
    if (entries[middle * 2] <= address) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  if (!low) {
    return false;
  }
  const uint32_t start = entries[(low - 1) * 2];
  const uint32_t packed = entries[(low - 1) * 2 + 1];
  // Gaps between functions carry no entry, so the nearest start below an
  // address does not have to be the one covering it.
  if (address - start >= ((packed >> 8) & 0x3FFFFF) * 4) {
    return false;
  }
  *out_start = start;
  *out_packed = packed;
  if (out_entry) {
    *out_entry = base + (low - 1) * 8;
  }
  return true;
}

// Walks the back chain, annotating each frame with its .pdata entry.
// A frame holds its entry sp in the back chain word and its saved lr 8 bytes
// below that. The packed word is logged raw so the decode stays checkable.
static std::string GetGuestBacktrace(PPCContext* context) {
  std::string trace;
  uint32_t sp = static_cast<uint32_t>(context->r[1]);
  uint32_t lr = static_cast<uint32_t>(context->lr);
  for (int frame = 0; frame < 24; ++frame) {
    trace.append(
        fmt::format("\n  #{:<2} lr=0x{:08X} sp=0x{:08X}", frame, lr, sp));
    uint32_t start = 0;
    uint32_t packed = 0;
    // See GuestFrame for why this steps back a byte.
    if (LookupGuestRuntimeFunction(lr ? lr - 1 : lr, &start, &packed)) {
      trace.append(fmt::format(
          " fn=0x{:08X} pdata=0x{:08X} prolog={} length={} eh={}", start,
          packed, packed & 0xFF, (packed >> 8) & 0x3FFFFF, packed >> 31));
      // The handler and its data sit in the two words ahead of the prolog.
      if ((packed >> 31) && IsGuestRangeReadable(start - 8, 8)) {
        auto* words =
            kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(start - 8);
        trace.append(fmt::format(" handler=0x{:08X} handler_data=0x{:08X}",
                                 uint32_t(words[0]), uint32_t(words[1])));
        trace.append(DescribeGuestFuncInfo(words[1]));
      }
    }
    if (!IsGuestRangeReadable(sp, 4)) {
      break;
    }
    uint32_t caller_sp =
        *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(sp);
    if (caller_sp <= sp || !IsGuestRangeReadable(caller_sp - 8, 4)) {
      break;
    }
    lr = *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(caller_sp - 8);
    sp = caller_sp;
  }
  return trace;
}

// Dumps guest stack words. The nonvolatile registers each frame saved are gone
// from the context by the time the throw lands, but they are still down here.
static std::string GetGuestStackDump(uint32_t sp, uint32_t word_count) {
  std::string dump;
  for (uint32_t i = 0; i < word_count; ++i) {
    const uint32_t address = sp + i * 4;
    if (!IsGuestRangeReadable(address, 4)) {
      break;
    }
    if (!(i % 8)) {
      dump.append(fmt::format("\n  {:08X}:", address));
    }
    dump.append(fmt::format(
        " {:08X}",
        uint32_t(
            *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(address))));
  }
  return dump;
}

// Dumps what each distinct pointer on the dumped stack refers to.
// The objects a frame was working on live off the stack, not on it.
static std::string GetGuestPointerDump(uint32_t sp, uint32_t word_count) {
  const uint32_t stack_end = sp + word_count * 4;
  std::vector<uint32_t> seen;
  std::string dump;
  for (uint32_t i = 0; i < word_count && seen.size() < 32; ++i) {
    const uint32_t address = sp + i * 4;
    if (!IsGuestRangeReadable(address, 4)) {
      break;
    }
    const uint32_t target =
        *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(address);
    // The stack itself is already dumped above.
    if ((target >= sp && target < stack_end) ||
        !IsGuestRangeReadable(target, 32) ||
        std::find(seen.begin(), seen.end(), target) != seen.end()) {
      continue;
    }
    seen.push_back(target);
    dump.append(fmt::format("\n  {:08X}:", target));
    for (uint32_t word = 0; word < 8; ++word) {
      dump.append(fmt::format(
          " {:08X}",
          uint32_t(*kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(
              target + word * 4))));
    }
  }
  return dump;
}

// One frame of the guest call chain.
// pc is where the frame resumes, always a return address, so it is looked up
// at pc - 1. A call ending a noreturn function returns onto the next function.
struct GuestFrame {
  uint32_t pc;
  uint32_t sp;
  uint32_t entry_sp;
  uint32_t function;
  uint32_t packed;
};

// Walks the back chain. False means it ran out of readable stack, leaving the
// last frame with no entry_sp and no way to unwind through it.
static bool WalkGuestFrames(PPCContext* context,
                            std::vector<GuestFrame>* out_frames) {
  uint32_t sp = static_cast<uint32_t>(context->r[1]);
  uint32_t pc = static_cast<uint32_t>(context->lr);
  for (int i = 0; i < 24; ++i) {
    GuestFrame frame = {};
    frame.pc = pc;
    frame.sp = sp;
    LookupGuestRuntimeFunction(pc ? pc - 1 : pc, &frame.function,
                               &frame.packed);
    if (!IsGuestRangeReadable(sp, 4)) {
      out_frames->push_back(frame);
      return false;
    }
    const uint32_t caller_sp = ReadGuestWord(sp);
    if (caller_sp <= sp || !IsGuestRangeReadable(caller_sp - 8, 4)) {
      out_frames->push_back(frame);
      return false;
    }
    frame.entry_sp = caller_sp;
    out_frames->push_back(frame);
    pc = ReadGuestWord(caller_sp - 8);
    sp = caller_sp;
  }
  return true;
}

// Undoes one frame's prolog, reloading the registers it saved on entry.
// Undoing frames outward leaves the context holding the state of the frame past
// the last one undone. A null context validates without writing anything.
// Bails on any prolog form but the plain stores, a save helper call included.
static bool UnwindGuestFrame(const GuestFrame& frame, PPCContext* context) {
  if (!frame.entry_sp || !frame.function) {
    return false;
  }
  uint32_t sp = frame.entry_sp;
  const uint32_t prolog_length = frame.packed & 0xFF;
  for (uint32_t i = 0; i < prolog_length; ++i) {
    const uint32_t address = frame.function + i * 4;
    if (!IsGuestRangeReadable(address, 4)) {
      return false;
    }
    const uint32_t instruction = ReadGuestWord(address);
    if (instruction == 0x7D8802A6) {  // mflr r12
      continue;
    }
    const uint32_t opcode = instruction >> 26;
    const uint32_t rs = (instruction >> 21) & 31;
    const uint32_t ra = (instruction >> 16) & 31;
    const int32_t displacement = static_cast<int16_t>(instruction & 0xFFFF);
    if (opcode == 37) {  // stwu r1, d(r1), the frame allocation
      if (ra != 1 || rs != 1) {
        return false;
      }
      sp += displacement;
      continue;
    }
    if (ra != 1) {
      return false;
    }
    if (opcode == 36) {  // stw rS, d(r1)
      const uint32_t at = sp + displacement;
      if (rs >= 13) {
        if (!IsGuestRangeReadable(at, 4)) {
          return false;
        }
        if (context) {
          context->r[rs] = ReadGuestWord(at);
        }
      }
      continue;
    }
    if (opcode == 62 && !(instruction & 3)) {  // std rS, ds(r1)
      const uint32_t at = sp + (displacement & ~int32_t(3));
      if (rs >= 13) {
        if (!IsGuestRangeReadable(at, 8)) {
          return false;
        }
        if (context) {
          context->r[rs] =
              (uint64_t(ReadGuestWord(at)) << 32) | ReadGuestWord(at + 4);
        }
      }
      continue;
    }
    if (opcode == 54) {  // stfd frS, d(r1)
      const uint32_t at = sp + displacement;
      if (rs >= 14) {
        if (!IsGuestRangeReadable(at, 8)) {
          return false;
        }
        if (context) {
          const uint64_t bits =
              (uint64_t(ReadGuestWord(at)) << 32) | ReadGuestWord(at + 4);
          std::memcpy(&context->f[rs], &bits, sizeof(bits));
        }
      }
      continue;
    }
    return false;
  }
  return true;
}

// Finds the catch(...) covering the call a frame is suspended at.
// Only an ellipsis handler is accepted, matching a thrown type against the
// catchable types would be a second step.
static bool FindGuestCatchAll(uint32_t func_info_ptr, uint32_t return_address,
                              uint32_t* out_handler, std::string* out_reason) {
  if (!IsGuestRangeReadable(func_info_ptr, sizeof(x_FuncInfo))) {
    *out_reason = "handler data is unreadable";
    return false;
  }
  auto* info = kernel_memory()->TranslateVirtual<x_FuncInfo*>(func_info_ptr);
  const uint32_t magic = info->magic_number;
  if (magic < 0x19930520 || magic > 0x19930522) {
    *out_reason = "handler data is not a FuncInfo";
    return false;
  }
  // The map is sorted by ip and each entry opens a state, so the last entry at
  // or below the call gives the state it was made in. Step back off the return
  // address, a state can begin on the instruction after the call.
  const uint32_t pc = return_address ? return_address - 1 : return_address;
  int32_t state = -1;
  int32_t ip_map_entry_count = info->ip_map_entry_count;
  if (ip_map_entry_count > 4096) {
    ip_map_entry_count = 4096;
  }
  const uint32_t ip_to_state_map_ptr = info->ip_to_state_map_ptr;
  for (int32_t i = 0; i < ip_map_entry_count; ++i) {
    const uint32_t at = ip_to_state_map_ptr +
                        uint32_t(i) * uint32_t(sizeof(x_IpToStateMapEntry));
    if (!IsGuestRangeReadable(at, sizeof(x_IpToStateMapEntry))) {
      break;
    }
    auto* entry = kernel_memory()->TranslateVirtual<x_IpToStateMapEntry*>(at);
    if (uint32_t(entry->ip) > pc) {
      break;
    }
    state = entry->state;
  }
  if (state < 0) {
    *out_reason = "no state covers the resume pc";
    return false;
  }
  int32_t try_block_count = info->try_block_count;
  if (try_block_count > 16) {
    try_block_count = 16;
  }
  const uint32_t try_block_map_ptr = info->try_block_map_ptr;
  for (int32_t i = 0; i < try_block_count; ++i) {
    const uint32_t try_ptr =
        try_block_map_ptr + uint32_t(i) * uint32_t(sizeof(x_TryBlockMapEntry));
    if (!IsGuestRangeReadable(try_ptr, sizeof(x_TryBlockMapEntry))) {
      break;
    }
    auto* try_block =
        kernel_memory()->TranslateVirtual<x_TryBlockMapEntry*>(try_ptr);
    if (state < int32_t(try_block->try_low) ||
        state > int32_t(try_block->try_high)) {
      continue;
    }
    int32_t catch_count = try_block->catch_count;
    if (catch_count > 16) {
      catch_count = 16;
    }
    const uint32_t handler_array_ptr = try_block->handler_array_ptr;
    for (int32_t j = 0; j < catch_count; ++j) {
      const uint32_t handler_ptr =
          handler_array_ptr + uint32_t(j) * uint32_t(sizeof(x_HandlerType));
      if (!IsGuestRangeReadable(handler_ptr, sizeof(x_HandlerType))) {
        break;
      }
      auto* handler =
          kernel_memory()->TranslateVirtual<x_HandlerType*>(handler_ptr);
      if (!uint32_t(handler->type_ptr)) {
        *out_handler = handler->handler_address;
        return true;
      }
    }
    *out_reason =
        fmt::format("try block {} at state {} has no catch(...)", i, state);
    return false;
  }
  *out_reason = fmt::format("no try block covers state {}", state);
  return false;
}

struct CppDispatchPlan {
  std::vector<GuestFrame> frames;
  size_t catcher = 0;
  uint32_t funclet = 0;
};

// Decides whether a guest C++ throw can reach a catch(...) in an ancestor.
// Only when the unwind needs no guest code of its own, so every frame in
// between must be handler free with a prolog we can undo. Pure analysis, which
// lets the plan be logged before anything acts on it.
static bool PlanCppDispatch(PPCContext* context, CppDispatchPlan* plan,
                            std::string* log, std::string* out_reason) {
  const bool walked = WalkGuestFrames(context, &plan->frames);
  auto& frames = plan->frames;
  size_t catcher = 0;
  while (catcher < frames.size() && !(frames[catcher].packed >> 31)) {
    if (!frames[catcher].function) {
      *out_reason = fmt::format("frame #{} pc=0x{:08X} has no pdata entry",
                                catcher, frames[catcher].pc);
      return false;
    }
    ++catcher;
  }
  if (catcher >= frames.size()) {
    *out_reason =
        fmt::format("no frame with a handler in {} frames{}", frames.size(),
                    walked ? "" : ", stack walk incomplete");
    return false;
  }
  plan->catcher = catcher;
  const GuestFrame& frame = frames[catcher];
  if (!IsGuestRangeReadable(frame.function - 8, 8)) {
    *out_reason = "the handler words are unreadable";
    return false;
  }
  if (!FindGuestCatchAll(ReadGuestWord(frame.function - 4), frame.pc,
                         &plan->funclet, out_reason)) {
    return false;
  }
  if (!IsGuestRangeReadable(plan->funclet, 4)) {
    *out_reason = fmt::format("funclet 0x{:08X} is not code", plan->funclet);
    return false;
  }
  for (size_t i = 0; i < catcher; ++i) {
    if (!UnwindGuestFrame(frames[i], nullptr)) {
      *out_reason =
          fmt::format("frame #{} fn=0x{:08X} has a prolog we cannot undo", i,
                      frames[i].function);
      return false;
    }
  }

  // What it takes to check the handoff, logged before the funclet runs. A
  // fault in there would otherwise take the whole message with it.
  log->append(fmt::format(
      "\n  dispatching to catch(...) 0x{:08X} for frame #{} fn=0x{:08X} "
      "sp=0x{:08X}",
      plan->funclet, catcher, frame.function, frame.sp));
  uint32_t funclet_start = 0;
  uint32_t funclet_packed = 0;
  if (LookupGuestRuntimeFunction(plan->funclet, &funclet_start,
                                 &funclet_packed)) {
    log->append(fmt::format(
        "\n  funclet fn=0x{:08X} pdata=0x{:08X} prolog={} length={} eh={}",
        funclet_start, funclet_packed, funclet_packed & 0xFF,
        (funclet_packed >> 8) & 0x3FFFFF, funclet_packed >> 31));
  }
  if (IsGuestRangeReadable(plan->funclet, 128)) {
    log->append("\n  funclet code:");
    for (uint32_t i = 0; i < 32; ++i) {
      if (!(i % 8)) {
        log->append(fmt::format("\n    {:08X}:", plan->funclet + i * 4));
      }
      log->append(fmt::format(" {:08X}", ReadGuestWord(plan->funclet + i * 4)));
    }
  }
  return true;
}

// Rebuilds the catching frame's registers, runs the funclet the way
// __CxxFrameHandler would, then points lr at the address it hands back so the
// import thunk lands there on the way out.
// That jump strands the host JIT frames between the throw and the catcher.
// Guest state lives in the context and the guest stack, so the only cost is
// host stack, a few frames per throw, never reclaimed for the thread's life.
static void ApplyCppDispatch(PPCContext* context, const CppDispatchPlan& plan) {
  auto* thread = XThread::GetCurrentThread();
  if (!thread) {
    XELOGE("Guest C++ exception dispatch: no current thread");
    return;
  }
  const GuestFrame& frame = plan.frames[plan.catcher];
  for (size_t i = 0; i < plan.catcher; ++i) {
    UnwindGuestFrame(plan.frames[i], context);
  }
  context->r[1] = frame.sp;
  // A catch funclet rebuilds the catching function's frame pointer from r12,
  // reusing the offset that function's own prolog used. So r12 has to hold the
  // establisher frame, the sp the catching function was entered on.
  context->r[12] = frame.entry_sp;
  // ExecuteRaw passes the sentinel as the return address argument but does not
  // put it in lr, and the funclet blrs to whatever lr holds.
  context->lr = 0xBCBCBCBC;

  std::string handover =
      fmt::format("r1=0x{:08X} r12=0x{:08X}", frame.sp, frame.entry_sp);
  for (uint32_t i = 13; i < 32; ++i) {
    handover.append(fmt::format(" r{}=0x{:08X}", i, uint32_t(context->r[i])));
  }
  // On record before the funclet runs, a fault in there loses anything later.
  XELOGD("Guest C++ exception dispatch: entering 0x{:08X} with {}",
         plan.funclet, handover);

  if (!kernel_state()->processor()->ExecuteRaw(thread->thread_state(),
                                               plan.funclet)) {
    XELOGE("Guest C++ exception dispatch: the funclet did not resolve");
    return;
  }
  const uint32_t continuation = static_cast<uint32_t>(context->r[3]);
  if (!IsGuestRangeReadable(continuation, 4)) {
    XELOGE(
        "Guest C++ exception dispatch: funclet returned 0x{:08X}, which is not "
        "readable code, so the resume point is unknown",
        continuation);
    return;
  }
  context->r[1] = frame.sp;
  context->lr = continuation;
  XELOGD("Guest C++ exception dispatch: resuming at 0x{:08X} with sp=0x{:08X}",
         continuation, frame.sp);
}

void HandleCppException(pointer_t<X_EXCEPTION_RECORD> record,
                        PPCContext* context) {
  // C++ exception.
  // https://blogs.msdn.com/b/oldnewthing/archive/2010/07/30/10044061.aspx
  // http://www.drdobbs.com/visual-c-exception-handling-instrumentat/184416600
  // http://www.openrce.org/articles/full_view/21

  assert_true(record->number_parameters == 3);
  assert_true(record->exception_information[0] == 0x19930520);

  const uint32_t thrown_ptr = record->exception_information[1];
  const uint32_t throw_info_ptr = record->exception_information[2];

  // One error line says a throw happened and what became of it. Everything
  // that explains it is detail, and a delivered throw is routine.
  std::string thrown_type_name;
  std::string message = fmt::format("\n  object=0x{:08X} throw_info=0x{:08X}",
                                    thrown_ptr, throw_info_ptr);

  // Vftable for a throw by value, the thrown pointer itself for a throw by
  // pointer, which the leading .PA of the mangled type name marks.
  if (IsGuestRangeReadable(thrown_ptr, 4)) {
    message.append(fmt::format(
        "\n  object[0]=0x{:08X}",
        uint32_t(*kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(
            thrown_ptr))));
  }

  if (IsGuestRangeReadable(throw_info_ptr, sizeof(x_s__ThrowInfo))) {
    auto* throw_info =
        kernel_memory()->TranslateVirtual<x_s__ThrowInfo*>(throw_info_ptr);
    message.append(fmt::format("\n  attributes=0x{:08X}",
                               uint32_t(throw_info->attributes)));

    // First entry is the most derived type, the rest are its bases.
    const uint32_t array_ptr = throw_info->catchable_type_array_ptr;
    if (IsGuestRangeReadable(array_ptr, sizeof(uint32_t))) {
      auto* catchable_types =
          kernel_memory()->TranslateVirtual<x_s__CatchableTypeArray*>(
              array_ptr);
      int32_t count = catchable_types->number_catchable_types;
      if (count > 32) {
        count = 32;
      }
      for (int32_t i = 0; i < count; ++i) {
        const uint32_t entry_ptr = array_ptr + 4 + i * 4;
        if (!IsGuestRangeReadable(entry_ptr, sizeof(uint32_t))) {
          break;
        }
        const uint32_t catchable_ptr =
            *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(entry_ptr);
        if (!IsGuestRangeReadable(catchable_ptr, sizeof(x_s__CatchableType))) {
          continue;
        }
        auto* catchable =
            kernel_memory()->TranslateVirtual<x_s__CatchableType*>(
                catchable_ptr);
        const uint32_t descriptor_ptr = catchable->type_ptr;
        auto name = ReadGuestTypeName(descriptor_ptr);
        if (!i) {
          thrown_type_name = name;
        }
        message.append(fmt::format(
            "\n  {}: {}", i == 0 ? "thrown type" : "  base type", name));
      }
    }
  }

  // Anything deriving from std::exception keeps the what() string right after
  // the vftable, so it is worth a look even though the layout is a guess. Only
  // for a throw by value, a leading .PA means the object is a pointer and the
  // word after it belongs to the caller.
  if (!thrown_type_name.starts_with(".PA") &&
      IsGuestRangeReadable(thrown_ptr + 4, 4)) {
    const uint32_t what_ptr =
        *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(thrown_ptr + 4);
    auto what = ReadGuestString(what_ptr, 256);
    if (!what.empty()) {
      message.append(fmt::format("\n  possible what(): \"{}\"", what));
    }
  }

  const uint32_t sp = static_cast<uint32_t>(context->r[1]);
  message.append("\nGuest backtrace at the throw:");
  message.append(GetGuestBacktrace(context));

  CppDispatchPlan plan;
  std::string reason;
  const bool dispatch = PlanCppDispatch(context, &plan, &message, &reason);
  if (!dispatch) {
    message.append("\nGuest stack at the throw:");
    message.append(GetGuestStackDump(sp, 192));
    message.append("\nPointed-to memory:");
    message.append(GetGuestPointerDump(sp, 192));
  }

  const auto type = thrown_type_name.empty() ? "of an unknown type"
                                             : thrown_type_name.c_str();
  if (dispatch) {
    XELOGE(
        "Guest threw a C++ exception {}, delivering it to the catch(...) at "
        "0x{:08X}",
        type, plan.funclet);
  } else {
    XELOGE(
        "Guest threw a C++ exception {} that cannot be delivered, {}. The "
        "guest will run on into the throw.",
        type, reason);
  }

  // xe::debugging::Break();
  // Flushed before any guest code runs, the funclet can fault.
  XELOGD("Guest C++ exception detail:{}", message);

  if (dispatch) {
    ApplyCppDispatch(context, plan);
  }
}

// Guest EXCEPTION_RECORD: code, flags, chained record, address, parameter
// count, then 15 information words.
constexpr uint32_t kExceptionRecordSize = 0x50;
constexpr uint32_t kExceptionNestedCall = 0x10;
// Guest DISPATCHER_CONTEXT, as the kernel's RtlDispatchException fills it:
// control pc, function entry, establisher frame, context record.
constexpr uint32_t kDispatcherContextSize = 0x10;
constexpr uint32_t kDispatcherEstablisherFrame = 0x8;
// A dispatch's records: the CONTEXT handlers get, the EXCEPTION_RECORD, the
// DISPATCHER_CONTEXT, then the registers the access was made with.
constexpr uint32_t kRecordsException = kContextSize;
constexpr uint32_t kRecordsDispatcher =
    kRecordsException + kExceptionRecordSize;
constexpr uint32_t kRecordsFaulting =
    kRecordsDispatcher + kDispatcherContextSize;
constexpr uint32_t kRecordsSize = kRecordsFaulting + kContextSize;
// What a language handler returns.
constexpr uint32_t kExceptionContinueExecution = 0;
constexpr uint32_t kExceptionContinueSearch = 1;
constexpr uint32_t kExceptionNestedException = 2;
// Frames searched for a handler, which can sit under a deep recursion.
constexpr int kDispatchFrames = 128;
// What a guest call returns to host code with when it has no caller.
constexpr uint32_t kHostReturnAddress = 0xBCBCBCBC;
// Faults in handlers dispatched in turn. The kernel has no such limit, this
// keeps a fault that recurs in its handler from overflowing the host stack.
constexpr size_t kDispatchNesting = 4;
// The kernel builds its trap frame this far below the sp a fault is taken
// at (80076064 in 17559). The handlers run below it, which leaves what a
// function keeps below its sp as it was.
constexpr uint32_t kTrapFrameSize = 0x700;

using ExceptionDispatch = XThread::ExceptionDispatch;

// A guest CONTEXT record's fields, as the kernel's unwinder reads and writes
// them.
struct ContextView {
  uint8_t* p;
  uint64_t gpr(uint32_t n) const {
    return xe::load_and_swap<uint64_t>(p + kContextGpr + 8 * n);
  }
  void set_gpr(uint32_t n, uint64_t value) {
    xe::store_and_swap<uint64_t>(p + kContextGpr + 8 * n, value);
  }
  uint32_t iar() const { return xe::load_and_swap<uint32_t>(p + kContextIar); }
  void set_iar(uint32_t value) {
    xe::store_and_swap<uint32_t>(p + kContextIar, value);
  }
  uint32_t lr() const { return xe::load_and_swap<uint32_t>(p + kContextLr); }
  void set_lr(uint32_t value) {
    xe::store_and_swap<uint32_t>(p + kContextLr, value);
  }
  void set_cr(uint32_t value) {
    xe::store_and_swap<uint32_t>(p + kContextCr, value);
  }
  void set_fpr(uint32_t n, uint64_t bits) {
    xe::store_and_swap<uint64_t>(p + kContextFpr + 8 * n, bits);
  }
};

static bool ReadGuestDoubleword(uint32_t address, uint64_t* out_value) {
  if (!IsGuestRangeReadable(address, 8)) {
    return false;
  }
  *out_value =
      (uint64_t(ReadGuestWord(address)) << 32) | ReadGuestWord(address + 4);
  return true;
}

// What an instruction means to the unwinder, which simulates a prolog or a
// register save helper backward, or an epilog forward.
enum class UnwindStep {
  kNone,
  kLr,             // mflr backward, mtlr forward
  kCr,             // mfcr backward, mtcrf 0xFF forward
  kLoad64,         // std backward
  kLoad32,         // stw backward, lwz forward
  kBackChain,      // stwu or stwux r1 backward
  kBackChainR12,   // the same before a save helper call, which sets r12
  kFpr,            // stfd backward, lfd forward
  kCopyForward,    // mr rA, rS backward
  kR1FromR12,      // mr r12, r1 before a save helper call
  kCopyBack,       // mr forward
  kR12Offset,      // addi r12, r1, d before a save helper call
  kAddress,        // addi forward
  kSaveHelper,     // bl to a register save helper
  kRestoreHelper,  // b to a register restore helper
  kReturn,         // blr forward
  kRfi,
};

// How a range of instructions is simulated: forward through an epilog, back
// from a save helper call to where its r12 base was set, back through a
// prolog, back through a save helper.
constexpr uint32_t kUnwindForward = 0;
constexpr uint32_t kUnwindHelperBase = 1;
constexpr uint32_t kUnwindProlog = 2;
constexpr uint32_t kUnwindHelper = 3;

// A branch to a register save or restore helper, whose .pdata entries have
// flags 0 and 2.
struct UnwindBranch {
  uint32_t target;
  uint32_t begin;
  uint32_t packed;
};

static uint32_t FunctionEnd(uint32_t begin, uint32_t packed) {
  return begin + ((packed >> 8) & 0x3FFFFF) * 4;
}

static UnwindStep ClassifyUnwindInstruction(uint32_t i, uint32_t mode,
                                            uint32_t pc, UnwindBranch* branch) {
  const uint32_t rs = (i >> 21) & 31;
  const uint32_t ra = (i >> 16) & 31;
  const uint32_t rb = (i >> 11) & 31;
  switch (((i >> 24) & 0xFC) | mode) {
    case 0x38:
      return UnwindStep::kAddress;
    case 0x39:
      return rs == 12 && ra == 1 ? UnwindStep::kR12Offset : UnwindStep::kNone;
    case 0x4A: {
      const uint32_t target =
          ((i & 2) ? 0 : pc) + (uint32_t(int32_t(i << 6) >> 6) & ~3u);
      // A call to a function saving lr itself is no save helper.
      if ((i & 1) && IsGuestRangeReadable(target, 4) &&
          (ReadGuestWord(target) & 0xFC1FFFFE) == 0x7C0802A6) {
        return UnwindStep::kNone;
      }
      if (!LookupGuestRuntimeFunction(target, &branch->begin,
                                      &branch->packed)) {
        return UnwindStep::kNone;
      }
      branch->target = target;
      switch (branch->packed >> 30) {
        case 0:
          return (i & 1) ? UnwindStep::kSaveHelper : UnwindStep::kNone;
        case 2:
          return (i & 1) ? UnwindStep::kNone : UnwindStep::kRestoreHelper;
      }
      return UnwindStep::kNone;
    }
    case 0x4C:
      return i == 0x4E800020 ? UnwindStep::kReturn : UnwindStep::kNone;
    case 0x4D:
    case 0x4E:
    case 0x4F:
      return (i & 0x7FE) == 0x64 ? UnwindStep::kRfi : UnwindStep::kNone;
    case 0x7C:
    case 0x7D:
    case 0x7E:
    case 0x7F:
      switch (((i << 1) & 0xFFC) | mode) {
        case 0x04E:
        case 0x04F:
          return UnwindStep::kCr;
        case 0x240:
          return (i & 0x000FF000) == 0x000FF000 ? UnwindStep::kCr
                                                : UnwindStep::kNone;
        case 0x2DD:
        case 0x2DE:
        case 0x2DF:
          if (rs != 1 || ra != 1) {
            return UnwindStep::kNone;
          }
          return mode == kUnwindHelperBase ? UnwindStep::kBackChainR12
                                           : UnwindStep::kBackChain;
        case 0x54E:
        case 0x74C:
          return (i & 0x001FF800) == 0x00080000 ? UnwindStep::kLr
                                                : UnwindStep::kNone;
        case 0x6F0:
          return rs == rb ? UnwindStep::kCopyBack : UnwindStep::kNone;
        case 0x6F1:
          return rs == rb && ra == 12 && rb == 1 ? UnwindStep::kR1FromR12
                                                 : UnwindStep::kNone;
        case 0x6F2:
        case 0x6F3:
          return rs == rb && rb != 1 ? UnwindStep::kCopyForward
                                     : UnwindStep::kNone;
      }
      return UnwindStep::kNone;
    case 0x80:
      return UnwindStep::kLoad32;
    case 0x92:
      return ra == 1 ? UnwindStep::kLoad32 : UnwindStep::kNone;
    case 0x93:
      return ra == 12 || ra == 1 ? UnwindStep::kLoad32 : UnwindStep::kNone;
    case 0x95:
    case 0x96:
    case 0x97:
      if (rs != 1 || ra != 1) {
        return UnwindStep::kNone;
      }
      return mode == kUnwindHelperBase ? UnwindStep::kBackChainR12
                                       : UnwindStep::kBackChain;
    case 0xC8:
      return UnwindStep::kFpr;
    case 0xDA:
      return ra == 1 ? UnwindStep::kFpr : UnwindStep::kNone;
    case 0xDB:
      return ra == 12 || ra == 1 ? UnwindStep::kFpr : UnwindStep::kNone;
    case 0xFA:
      return (i & 3) == 0 && ra == 1 ? UnwindStep::kLoad64 : UnwindStep::kNone;
    case 0xFB:
      return (i & 3) == 0 && (ra == 12 || ra == 1) ? UnwindStep::kLoad64
                                                   : UnwindStep::kNone;
  }
  return UnwindStep::kNone;
}

// Undoes the frame of the function the .pdata entry (begin, packed) is for,
// suspended at control_pc, in the CONTEXT record. Returns where its caller
// resumes: the call there, or with no entry, lr - 4 for a leaf. False if it
// read past readable memory.
static bool VirtualUnwind(uint32_t control_pc, bool has_entry, uint32_t begin,
                          uint32_t packed, uint8_t* context,
                          bool* out_in_function, uint32_t* out_establisher,
                          uint32_t* out_next_pc) {
  ContextView c{context};
  if (!has_entry) {
    *out_next_pc = c.lr() - 4;
    return true;
  }
  *out_establisher = uint32_t(c.gpr(1));
  if (!IsGuestRangeReadable(control_pc, 4)) {
    return false;
  }
  const uint32_t instruction = ReadGuestWord(control_pc);
  if (instruction == 0x4E800020) {
    *out_in_function = false;
    *out_next_pc = c.lr();
    return true;
  }
  struct Range {
    uint32_t start;
    uint32_t end;
    int32_t step;
    uint32_t mode;
  };
  Range ranges[3];
  UnwindBranch branch = {};
  const uint32_t prolog_end = begin + (packed & 0xFF) * 4;
  if (ClassifyUnwindInstruction(instruction, kUnwindProlog, control_pc,
                                &branch) == UnwindStep::kRestoreHelper) {
    ranges[0] = {branch.target, FunctionEnd(branch.begin, branch.packed), 4,
                 kUnwindForward};
    *out_in_function = false;
  } else if ((packed >> 30) == 2) {
    // In a register restore helper.
    ranges[0] = {control_pc, FunctionEnd(begin, packed), 4, kUnwindForward};
    *out_in_function = false;
  } else if (control_pc >= begin && control_pc < prolog_end) {
    ranges[0] = {control_pc - 4, begin - 4, -4, kUnwindProlog};
    *out_in_function = false;
  } else {
    ranges[0] = {prolog_end - 4, begin - 4, -4, kUnwindProlog};
    *out_in_function = true;
  }

  uint32_t next_pc = c.lr() - 4;
  bool lr_restored = false;
  bool frame_restored = false;
  bool r12_from_back_chain = false;
  int top = 0;
  while (top >= 0) {
    Range& range = ranges[top];
    bool pushed = false;
    for (uint32_t pc = range.start; pc != range.end; pc += range.step) {
      if (!IsGuestRangeReadable(pc, 4)) {
        return false;
      }
      const uint32_t i = ReadGuestWord(pc);
      const uint32_t rs = (i >> 21) & 31;
      const uint32_t ra = (i >> 16) & 31;
      const uint64_t ea = c.gpr(ra) + int64_t(int16_t(i));
      uint64_t value = 0;
      bool ends_range = false;
      switch (ClassifyUnwindInstruction(i, range.mode, pc, &branch)) {
        case UnwindStep::kLr: {
          const uint32_t lr = uint32_t(c.gpr(rs));
          c.set_lr(lr);
          next_pc = lr_restored ? next_pc + 4 : lr - 4;
          lr_restored = true;
          break;
        }
        case UnwindStep::kCr:
          c.set_cr(uint32_t(c.gpr(rs)));
          break;
        case UnwindStep::kLoad64:
          if (!ReadGuestDoubleword(uint32_t(ea), &value)) {
            return false;
          }
          c.set_gpr(rs, value);
          break;
        case UnwindStep::kLoad32:
          if (!IsGuestRangeReadable(uint32_t(ea), 4)) {
            return false;
          }
          c.set_gpr(rs, ReadGuestWord(uint32_t(ea)));
          break;
        case UnwindStep::kBackChain:
        case UnwindStep::kBackChainR12: {
          const uint32_t sp = uint32_t(c.gpr(1));
          if (!IsGuestRangeReadable(sp, 4)) {
            return false;
          }
          if (range.mode == kUnwindHelperBase) {
            r12_from_back_chain = true;
            c.set_gpr(12, ReadGuestWord(sp));
            break;
          }
          c.set_gpr(1, ReadGuestWord(sp));
          if (!frame_restored) {
            frame_restored = true;
            *out_establisher = uint32_t(c.gpr(1));
          }
          break;
        }
        case UnwindStep::kFpr:
          if (!ReadGuestDoubleword(uint32_t(ea), &value)) {
            return false;
          }
          c.set_fpr(rs, value);
          break;
        case UnwindStep::kCopyForward:
          c.set_gpr(ra, c.gpr(rs));
          break;
        case UnwindStep::kR1FromR12:
          c.set_gpr(rs, c.gpr(ra));
          ends_range = true;
          break;
        case UnwindStep::kCopyBack:
          c.set_gpr(rs, c.gpr(ra));
          break;
        case UnwindStep::kR12Offset:
          if (!r12_from_back_chain) {
            c.set_gpr(rs, c.gpr(1));
          }
          c.set_gpr(rs, c.gpr(rs) + int64_t(int16_t(i)));
          ends_range = true;
          break;
        case UnwindStep::kAddress:
          c.set_gpr(rs, uint32_t(ea));
          break;
        case UnwindStep::kSaveHelper:
          // The helper's stores from its end back to where the call entered
          // it, with the r12 found scanning back from the call, then the rest
          // of the prolog.
          if (top) {
            return false;
          }
          ranges[1] = {FunctionEnd(branch.begin, branch.packed) - 4,
                       branch.target - 4, -4, kUnwindHelper};
          ranges[2] = {pc - 4, range.end, -4, kUnwindHelperBase};
          range.start = pc - 4;
          r12_from_back_chain = false;
          top = 2;
          pushed = true;
          break;
        case UnwindStep::kReturn:
          next_pc = c.lr() - 4;
          ends_range = true;
          break;
        case UnwindStep::kRfi:
          *out_establisher = uint32_t(c.gpr(1));
          break;
        default:
          break;
      }
      if (ends_range || pushed) {
        break;
      }
    }
    if (!pushed) {
      --top;
    }
  }
  *out_next_pc = next_pc;
  return true;
}

// The innermost of the first count dispatches that called a handler on sp to
// return to pc + 4, which is where unwinding the handler's frame leads, else
// count.
static size_t FindDispatchReturnedTo(
    const std::vector<ExceptionDispatch>& dispatches, size_t count, uint32_t pc,
    uint32_t sp) {
  for (size_t i = count; i-- > 0;) {
    if (pc + 4 == dispatches[i].handler_return &&
        sp == dispatches[i].handler_sp) {
      return i;
    }
  }
  return count;
}

// Steps through the frames a dispatch searches one at a time, as
// RtlDispatchException does: each is looked up at its control pc and unwound
// in a copy of the CONTEXT, which gives its establisher frame and its caller's
// control pc. A frame unwinding to an outer dispatch's handler call goes on
// from the registers that dispatch's fault was taken with, where the kernel's
// frames down to its trap frame would lead. Nothing is left to free when a
// handler doesn't return.
struct DispatchWalk {
  const std::vector<ExceptionDispatch>* outer;
  // Outer dispatches the walk can still meet, innermost last.
  size_t unmet;
  uint32_t stack_base;
  uint8_t context[kContextSize];
  // The frame: where it's suspended, its sp, its .pdata entry, whether its
  // prolog has run, its establisher frame and its caller's control pc.
  uint32_t control_pc;
  uint32_t sp;
  bool has_entry;
  uint32_t function_entry;
  uint32_t begin;
  uint32_t packed;
  bool in_function;
  uint32_t establisher;
  uint32_t next_pc;
  // False when it has no caller.
  bool more;
  // One more than the index of the outer dispatch the frame was met through,
  // else 0.
  size_t met;
  int count;
};

// Looks up and unwinds the frame at control_pc. False if that read past
// readable memory.
static bool UnwindDispatchFrame(DispatchWalk* walk) {
  walk->sp = uint32_t(ContextView{walk->context}.gpr(1));
  walk->function_entry = 0;
  walk->begin = 0;
  walk->packed = 0;
  walk->in_function = false;
  walk->establisher = walk->sp;
  walk->more = false;
  walk->has_entry = LookupGuestRuntimeFunction(
      walk->control_pc, &walk->begin, &walk->packed, &walk->function_entry);
  if (!VirtualUnwind(walk->control_pc, walk->has_entry, walk->begin,
                     walk->packed, walk->context, &walk->in_function,
                     &walk->establisher, &walk->next_pc)) {
    return false;
  }
  // With no entry the caller is found from lr, which stays as it was.
  walk->more = walk->has_entry || walk->next_pc != walk->control_pc;
  return true;
}

// Starts at the frame the fault was taken in, from its CONTEXT.
static bool StartDispatchWalk(DispatchWalk* walk,
                              const std::vector<ExceptionDispatch>& outer,
                              size_t outer_count, const uint8_t* context,
                              uint32_t stack_base) {
  walk->outer = &outer;
  walk->unmet = outer_count;
  walk->stack_base = stack_base;
  std::memcpy(walk->context, context, kContextSize);
  walk->control_pc = ContextView{walk->context}.iar();
  walk->met = 0;
  walk->count = 1;
  return UnwindDispatchFrame(walk);
}

// Moves on to the frame's caller. False once the stack is unwound, or where
// the walk can't go on.
static bool NextDispatchFrame(DispatchWalk* walk) {
  ContextView c{walk->context};
  if (!walk->more || walk->count >= kDispatchFrames ||
      uint32_t(c.gpr(1)) >= walk->stack_base) {
    return false;
  }
  walk->control_pc = walk->next_pc;
  walk->met = 0;
  const size_t i = FindDispatchReturnedTo(*walk->outer, walk->unmet,
                                          walk->control_pc, uint32_t(c.gpr(1)));
  if (i < walk->unmet) {
    walk->unmet = i;
    walk->met = i + 1;
    std::memcpy(walk->context,
                kernel_memory()->TranslateVirtual<uint8_t*>(
                    (*walk->outer)[i].records + kRecordsFaulting),
                kContextSize);
    walk->control_pc = c.iar();
  }
  ++walk->count;
  return UnwindDispatchFrame(walk);
}

// Drops the dispatches the thread has left, with their records. A walk from
// the fault that gets past the sp a handler was called on without meeting it
// shows that handler isn't running anymore.
static void DropLeftDispatches(std::vector<ExceptionDispatch>* dispatches,
                               const uint8_t* context, uint32_t stack_base) {
  if (dispatches->empty()) {
    return;
  }
  DispatchWalk walk = {};
  uint32_t reached_sp = 0;
  for (bool more = StartDispatchWalk(&walk, *dispatches, dispatches->size(),
                                     context, stack_base);
       more && !walk.met; more = NextDispatchFrame(&walk)) {
    reached_sp = walk.sp;
  }
  size_t live = walk.met;
  if (!live) {
    live = dispatches->size();
    while (live && (*dispatches)[live - 1].handler_sp <= reached_sp) {
      --live;
    }
  }
  for (size_t i = live; i < dispatches->size(); ++i) {
    kernel_memory()->SystemHeapFree((*dispatches)[i].records);
  }
  dispatches->resize(live);
}

// Delivers an access violation at the instruction iar to the guest's
// frame-based handlers, searching the frames from the one it was taken in as
// RtlDispatchException does. The handlers return to handler_return. Returns
// the address a handler continued at, with the context loaded from its CONTEXT
// record, or 0 if none did, with the context as it was.
static uint32_t DispatchAccessViolation(PPCContext* context,
                                        uint32_t handler_return, uint32_t iar,
                                        bool is_store, uint32_t address,
                                        const char* fault) {
  auto* thread = XThread::GetCurrentThread();
  auto* thread_state = context->thread_state;
  if (!thread || !thread_state) {
    return 0;
  }
  uint32_t stack_end = 0;
  uint32_t stack_base = 0;
  xeRtlGetStackLimits(context, &stack_end, &stack_base);
  const uint32_t sp = static_cast<uint32_t>(context->r[1]);
  if (sp < stack_end + kTrapFrameSize || sp > stack_base ||
      !IsGuestRangeReadable(sp - kTrapFrameSize, 4)) {
    XELOGE("{} at {:08X}: no stack for the handlers below {:08X}", fault,
           address, sp);
    return 0;
  }

  // A handler that walks the stack, as the CLR's does, unwinds through real
  // frames, so the records can't go on the stack and go in the system heap
  // instead. On the console the walk passes RtlpExecuteHandlerForException and
  // the kernel's dispatch frames first, which have no guest code here.
  ExceptionDispatch dispatch = {};
  dispatch.handler_sp = sp - kTrapFrameSize;
  dispatch.handler_return = handler_return;
  dispatch.records = kernel_memory()->SystemHeapAlloc(kRecordsSize, 16);
  if (!dispatch.records) {
    XELOGE("{} at {:08X}: no memory for the records", fault, address);
    return 0;
  }
  const uint32_t context_record = dispatch.records;
  const uint32_t exception_record = dispatch.records + kRecordsException;
  const uint32_t dispatcher_context = dispatch.records + kRecordsDispatcher;
  auto* records = kernel_memory()->TranslateVirtual<uint8_t*>(dispatch.records);
  StoreContextRecord(records, context, iar, static_cast<uint32_t>(context->lr));
  std::memcpy(records + kRecordsFaulting, records, kContextSize);

  auto& dispatches = thread->exception_dispatches();
  DropLeftDispatches(&dispatches, records + kRecordsFaulting, stack_base);
  if (dispatches.size() >= kDispatchNesting) {
    XELOGE("{} at {:08X}: already in {} handlers", fault, address,
           dispatches.size());
    kernel_memory()->SystemHeapFree(dispatch.records);
    return 0;
  }
  // The trap frame's back chain, to the sp the fault was taken at.
  xe::store_and_swap<uint32_t>(
      kernel_memory()->TranslateVirtual<uint8_t*>(dispatch.handler_sp), sp);

  // The record the kernel's storage fault handler builds (the trap dispatch at
  // 80076BA0 in 17559): no flags or chained record, the faulting instruction's
  // address, and two parameters, whether it was a store (from the DSISR) and
  // the data address (the DAR). Fetching an instruction is no store, and its
  // address is both.
  auto* record = records + kRecordsException;
  std::memset(record, 0, kExceptionRecordSize);
  xe::store_and_swap<uint32_t>(record + 0x0, X_STATUS_ACCESS_VIOLATION);
  xe::store_and_swap<uint32_t>(record + 0xC, iar);
  xe::store_and_swap<uint32_t>(record + 0x10, 2);
  xe::store_and_swap<uint32_t>(record + 0x14, is_store ? 1 : 0);
  xe::store_and_swap<uint32_t>(record + 0x18, address);

  const size_t index = dispatches.size();
  dispatches.push_back(dispatch);

  uint32_t resume_address = 0;
  uint32_t flags = 0;
  uint32_t nested_frame = 0;
  DispatchWalk walk = {};
  for (bool more = StartDispatchWalk(&walk, dispatches, index,
                                     records + kRecordsFaulting, stack_base);
       more; more = NextDispatchFrame(&walk)) {
    if (walk.met) {
      // A fault in an outer dispatch's handler is nested in that dispatch's
      // frames.
      flags |= kExceptionNestedCall;
      nested_frame = std::max(
          nested_frame,
          ReadGuestWord(dispatches[walk.met - 1].records + kRecordsDispatcher +
                        kDispatcherEstablisherFrame));
    }
    if (!walk.has_entry) {
      continue;
    }
    // As RtlDispatchException: a frame outside the stack ends the search.
    const uint32_t establisher = walk.establisher;
    if (establisher < stack_end || establisher > stack_base ||
        (establisher & 7)) {
      break;
    }
    // A frame has a handler when both of its entry's top bits are set, once
    // its prolog has run, and the handler sits two words ahead of the
    // function.
    if ((walk.packed >> 30) != 3 || !walk.in_function ||
        !IsGuestRangeReadable(walk.begin - 8, 4)) {
      continue;
    }
    const uint32_t handler = ReadGuestWord(walk.begin - 8);
    auto* handler_function =
        handler ? kernel_state()->processor()->ResolveFunction(handler)
                : nullptr;
    if (!handler_function) {
      continue;
    }
    auto* dispatcher = records + kRecordsDispatcher;
    xe::store_and_swap<uint32_t>(dispatcher + 0x0, walk.control_pc);
    xe::store_and_swap<uint32_t>(dispatcher + 0x4, walk.function_entry);
    xe::store_and_swap<uint32_t>(dispatcher + kDispatcherEstablisherFrame,
                                 establisher);
    xe::store_and_swap<uint32_t>(dispatcher + 0xC, context_record);
    xe::store_and_swap<uint32_t>(record + 0x4, flags);

    context->r[1] = dispatch.handler_sp;
    context->r[3] = exception_record;
    context->r[4] = establisher;
    context->r[5] = context_record;
    context->r[6] = dispatcher_context;
    context->lr = dispatch.handler_return;
    if (!handler_function->Call(thread_state, dispatch.handler_return)) {
      XELOGE("{} at {:08X}: the handler at {:08X} didn't run", fault, address,
             handler);
      break;
    }
    if (index >= dispatches.size() ||
        dispatches[index].records != dispatch.records) {
      XELOGE("{} at {:08X}: dropped while the handler at {:08X} ran", fault,
             address, handler);
      return 0;
    }
    if (nested_frame == establisher) {
      flags &= ~kExceptionNestedCall;
      nested_frame = 0;
    }
    const uint32_t disposition = static_cast<uint32_t>(context->r[3]);
    if (disposition == kExceptionContinueExecution) {
      resume_address = LoadContextRecord(records, context);
      XELOGD(
          "{} at {:08X}: the handler at {:08X} of frame #{} continued at "
          "{:08X}",
          fault, address, handler, walk.count - 1, resume_address);
      break;
    }
    if (disposition == kExceptionNestedException) {
      flags |= kExceptionNestedCall;
      nested_frame =
          std::max(nested_frame, xe::load_and_swap<uint32_t>(
                                     dispatcher + kDispatcherEstablisherFrame));
    } else if (disposition != kExceptionContinueSearch) {
      XELOGE(
          "{} at {:08X}: the handler at {:08X} returned {}, which isn't "
          "supported",
          fault, address, handler, disposition);
      break;
    }
  }

  if (!resume_address) {
    LoadContextRecord(records + kRecordsFaulting, context);
    XELOGE("{} at {:08X} by {:08X}: no handler in {} frames continued it",
           fault, address, iar, walk.count);
  }
  for (size_t i = index; i < dispatches.size(); ++i) {
    kernel_memory()->SystemHeapFree(dispatches[i].records);
  }
  dispatches.resize(index);
  return resume_address;
}

uint32_t DispatchCodeFetchFault(PPCContext* context, uint32_t address) {
  if (!XThread::IsInThread()) {
    return 0;
  }
  // The call never ran, so lr holds its return address and r1 the caller's
  // stack pointer. The handlers return as if the faulting call had called
  // them.
  const uint32_t return_address = static_cast<uint32_t>(context->lr);
  const uint32_t resume_address = DispatchAccessViolation(
      context, return_address ? return_address : kHostReturnAddress, address,
      false, address, "Code fetch fault");
  if (!resume_address) {
    kernel_state()->emulator()->HaltCrashedGuestThread(
        address, fmt::format("Access Violation: fetch at 0x{:08X}", address));
  }
  return resume_address;
}

uint32_t DispatchDataFault(PPCContext* context, uint32_t pc, bool is_store,
                           uint32_t address) {
  // The search starts at the function the access is in, at the access, as the
  // kernel hands RtlDispatchException the trap's context. The handlers return
  // as if the access had called them, so a handler that walks the stack finds
  // the faulting function first.
  return DispatchAccessViolation(context, pc + 4, pc, is_store, address,
                                 "Data fault");
}

// Where an unwind leaves a frame for one returning to a handler the dispatcher
// called, it goes on from the registers the access or call faulted with. The
// CONTEXT keeps its iar.
static void MeetDispatchForUnwind(XThread* thread, uint8_t* context,
                                  uint32_t* pc) {
  ContextView c{context};
  const auto& dispatches = thread->exception_dispatches();
  const size_t i = FindDispatchReturnedTo(dispatches, dispatches.size(), *pc,
                                          uint32_t(c.gpr(1)));
  if (i == dispatches.size()) {
    return;
  }
  const uint32_t iar = c.iar();
  std::memcpy(context,
              kernel_memory()->TranslateVirtual<uint8_t*>(
                  dispatches[i].records + kRecordsFaulting),
              kContextSize);
  *pc = c.iar();
  c.set_iar(iar);
}

// Calls guest code from a kernel function, on the stack at sp, returning to the
// kernel.
static bool CallGuestFromKernel(PPCContext* ctx, uint32_t address,
                                uint32_t sp) {
  auto* function = kernel_state()->processor()->ResolveFunction(address);
  if (!function) {
    return false;
  }
  ctx->r[1] = sp;
  ctx->lr = kHostReturnAddress;
  return function->Call(ctx->thread_state, kHostReturnAddress);
}

// RtlUnwind2's frame on the guest stack: a back chain, the record it makes
// when it's given none, its dispatcher context, then the CONTEXT RtlUnwind
// gives it.
constexpr uint32_t kUnwindRecord = 0x10;
constexpr uint32_t kUnwindDispatcher = kUnwindRecord + kExceptionRecordSize;
constexpr uint32_t kUnwindContext = kUnwindDispatcher + kDispatcherContextSize;
constexpr uint32_t kUnwindFrameSize = kUnwindContext + kContextSize;
// Flags of a record being unwound, the frame it's unwound to and an unwind with
// no frame to stop at.
constexpr uint32_t kExceptionUnwinding = 0x2;
constexpr uint32_t kExceptionExitUnwind = 0x4;
constexpr uint32_t kExceptionTargetUnwind = 0x20;

// Unwinds the frames from the caller to the one whose establisher frame is
// target_frame, calling the handlers on the way. Then the guest continues at
// target_ip in that frame with return_value in r3, through lr when the import
// thunk returns. With no record one is made, and with no CONTEXT record
// RtlUnwind's own is used.
static void UnwindGuestFrames(PPCContext* ctx, uint32_t target_frame,
                              uint32_t target_ip, uint32_t record,
                              uint32_t return_value, uint32_t context_record) {
  auto* thread = XThread::GetCurrentThread();
  if (!thread || !ctx->thread_state) {
    return;
  }
  const uint32_t caller_sp = uint32_t(ctx->r[1]);
  const uint32_t caller_lr = uint32_t(ctx->lr);
  const uint32_t frame = (caller_sp - 0x10 - kUnwindFrameSize) & ~0xFu;
  if (!IsGuestRangeReadable(frame, kUnwindFrameSize)) {
    XELOGE("Unwind to {:08X}: no stack for the unwind", target_frame);
    return;
  }
  xe::store_and_swap<uint32_t>(
      kernel_memory()->TranslateVirtual<uint8_t*>(frame), caller_sp);
  if (!context_record) {
    context_record = frame + kUnwindContext;
  }
  auto* context = kernel_memory()->TranslateVirtual<uint8_t*>(context_record);
  StoreContextRecord(context, ctx, target_ip, caller_lr);
  ContextView c{context};

  uint32_t control_pc = caller_lr - 4;
  if (!record) {
    record = frame + kUnwindRecord;
    auto* made = kernel_memory()->TranslateVirtual<uint8_t*>(record);
    std::memset(made, 0, kExceptionRecordSize);
    xe::store_and_swap<uint32_t>(made + 0x0, X_STATUS_UNWIND);
    xe::store_and_swap<uint32_t>(made + 0xC, control_pc);
  }
  auto* record_bytes = kernel_memory()->TranslateVirtual<uint8_t*>(record);
  if (!target_frame) {
    xe::store_and_swap<uint32_t>(
        record_bytes + 0x4,
        xe::load_and_swap<uint32_t>(record_bytes + 0x4) | kExceptionExitUnwind);
  }
  MeetDispatchForUnwind(thread, context, &control_pc);

  uint32_t stack_end = 0;
  uint32_t stack_base = 0;
  xeRtlGetStackLimits(ctx, &stack_end, &stack_base);
  const uint32_t dispatcher = frame + kUnwindDispatcher;
  auto* dispatcher_bytes =
      kernel_memory()->TranslateVirtual<uint8_t*>(dispatcher);
  uint8_t probe[kContextSize];
  uint32_t establisher = caller_sp;
  X_STATUS failure = X_STATUS_SUCCESS;
  while (true) {
    uint32_t begin = 0;
    uint32_t packed = 0;
    uint32_t function_entry = 0;
    const bool has_entry = LookupGuestRuntimeFunction(control_pc, &begin,
                                                      &packed, &function_entry);
    bool in_function = false;
    uint32_t next_pc = 0;
    if (has_entry) {
      std::memcpy(probe, context, kContextSize);
      if (!VirtualUnwind(control_pc, true, begin, packed, probe, &in_function,
                         &establisher, &next_pc)) {
        failure = X_STATUS_BAD_STACK;
        break;
      }
      if (establisher < stack_end || establisher > stack_base ||
          (target_frame && target_frame < establisher) || (establisher & 7)) {
        failure = X_STATUS_BAD_STACK;
        break;
      }
      if ((packed >> 30) == 3 && in_function &&
          IsGuestRangeReadable(begin - 8, 4)) {
        xe::store_and_swap<uint32_t>(dispatcher_bytes + 0x0, control_pc);
        xe::store_and_swap<uint32_t>(dispatcher_bytes + 0x4, function_entry);
        xe::store_and_swap<uint32_t>(
            dispatcher_bytes + kDispatcherEstablisherFrame, establisher);
        xe::store_and_swap<uint32_t>(dispatcher_bytes + 0xC, context_record);
        xe::store_and_swap<uint32_t>(
            record_bytes + 0x4,
            kExceptionUnwinding |
                (establisher == target_frame ? kExceptionTargetUnwind : 0));
        c.set_gpr(3, return_value);
        const uint32_t handler = ReadGuestWord(begin - 8);
        ctx->r[3] = record;
        ctx->r[4] = establisher;
        ctx->r[5] = context_record;
        ctx->r[6] = dispatcher;
        if (!CallGuestFromKernel(ctx, handler, frame)) {
          XELOGE("Unwind to {:08X}: the handler at {:08X} didn't run",
                 target_frame, handler);
          failure = X_STATUS_INVALID_DISPOSITION;
          break;
        }
        // A collided unwind, 3, isn't supported.
        if (uint32_t(ctx->r[3]) != kExceptionContinueSearch) {
          failure = X_STATUS_INVALID_DISPOSITION;
          break;
        }
      }
      if (establisher == target_frame) {
        break;
      }
      if (!VirtualUnwind(control_pc, true, begin, packed, context, &in_function,
                         &establisher, &next_pc)) {
        failure = X_STATUS_BAD_STACK;
        break;
      }
    } else {
      VirtualUnwind(control_pc, false, 0, 0, context, &in_function,
                    &establisher, &next_pc);
      if (next_pc == control_pc) {
        failure = X_STATUS_BAD_FUNCTION_TABLE;
        break;
      }
    }
    MeetDispatchForUnwind(thread, context, &next_pc);
    control_pc = next_pc;
    if (establisher >= stack_base) {
      break;
    }
  }
  if (failure == X_STATUS_SUCCESS && establisher != target_frame) {
    // An exit unwind, or one that never found its frame, raises the record
    // again.
    failure = xe::load_and_swap<uint32_t>(record_bytes);
  }
  if (failure != X_STATUS_SUCCESS) {
    kernel_state()->emulator()->HaltCrashedGuestThread(
        control_pc, fmt::format("Unwind to {:08X} at {:08X} failed: {:08X}",
                                target_frame, target_ip, failure));
  }
  c.set_gpr(3, return_value);
  ctx->lr = LoadContextRecord(context, ctx);
}

void RtlUnwind_entry(dword_t target_frame, dword_t target_ip,
                     pointer_t<X_EXCEPTION_RECORD> record, dword_t return_value,
                     const ppc_context_t& ctx) {
  UnwindGuestFrames(ctx, target_frame, target_ip, record.guest_address(),
                    return_value, 0);
}
DECLARE_XBOXKRNL_EXPORT1(RtlUnwind, kNone, kImplemented);

void RtlUnwind2_entry(dword_t target_frame, dword_t target_ip,
                      pointer_t<X_EXCEPTION_RECORD> record,
                      dword_t return_value, lpvoid_t context,
                      const ppc_context_t& ctx) {
  UnwindGuestFrames(ctx, target_frame, target_ip, record.guest_address(),
                    return_value, context.guest_address());
}
DECLARE_XBOXKRNL_EXPORT1(RtlUnwind2, kNone, kImplemented);

// The language handler of functions with __try blocks. Their scope table, the
// handler data, has a count, then for each scope its start and end, its filter
// or finally handler, and its except label, or 0 for a finally handler.
void __C_specific_handler_entry(pointer_t<X_EXCEPTION_RECORD> record,
                                dword_t establisher, lpvoid_t context,
                                lpvoid_t dispatcher_context,
                                const ppc_context_t& ctx) {
  auto* dispatcher = dispatcher_context.as<uint8_t*>();
  const uint32_t control_pc = xe::load_and_swap<uint32_t>(dispatcher + 0x0);
  const uint32_t function_entry = xe::load_and_swap<uint32_t>(dispatcher + 0x4);
  uint32_t count = 0;
  uint32_t scopes = 0;
  if (IsGuestRangeReadable(function_entry, 4)) {
    const uint32_t begin = ReadGuestWord(function_entry);
    if (IsGuestRangeReadable(begin - 4, 4)) {
      const uint32_t table = ReadGuestWord(begin - 4);
      if (IsGuestRangeReadable(table, 4)) {
        count = ReadGuestWord(table);
        scopes = table + 4;
      }
    }
  }
  if (count && !IsGuestRangeReadable(scopes, count * 0x10)) {
    count = 0;
  }
  // The registers a caller keeps, saved around the filters and finally
  // handlers.
  uint64_t saved_gprs[32];
  double saved_fprs[32];
  std::memcpy(saved_gprs, ctx->r, sizeof(saved_gprs));
  std::memcpy(saved_fprs, ctx->f, sizeof(saved_fprs));
  const uint64_t saved_lr = ctx->lr;
  const uint64_t saved_cr = ctx->cr();
  auto restore = [&]() {
    std::memcpy(ctx->r, saved_gprs, sizeof(saved_gprs));
    std::memcpy(ctx->f, saved_fprs, sizeof(saved_fprs));
    ctx->lr = saved_lr;
    ctx->set_cr(saved_cr);
  };
  auto finish = [&](uint32_t disposition) {
    restore();
    ctx->r[3] = disposition;
  };
  // Its frame, with the EXCEPTION_POINTERS filters get.
  const uint32_t sp = uint32_t(ctx->r[1]);
  const uint32_t frame = (sp - 0xA0) & ~0xFu;
  const uint32_t pointers = frame + 0x50;
  if (!IsGuestRangeReadable(frame, 0xA0)) {
    finish(kExceptionContinueSearch);
    return;
  }
  auto* frame_bytes = kernel_memory()->TranslateVirtual<uint8_t*>(frame);
  xe::store_and_swap<uint32_t>(frame_bytes, sp);
  xe::store_and_swap<uint32_t>(frame_bytes + 0x50, record.guest_address());
  xe::store_and_swap<uint32_t>(frame_bytes + 0x54, context.guest_address());

  auto scope_word = [&](uint32_t index, uint32_t offset) {
    return ReadGuestWord(scopes + index * 0x10 + offset);
  };
  if (!(record->exception_flags & 0x66)) {
    for (uint32_t i = 0; i < count; ++i) {
      if (control_pc < scope_word(i, 0) || control_pc >= scope_word(i, 4) ||
          !scope_word(i, 0xC)) {
        continue;
      }
      ctx->r[3] = pointers;
      ctx->r[12] = establisher;
      if (!CallGuestFromKernel(ctx, scope_word(i, 8), frame)) {
        XELOGE("__C_specific_handler: no filter at {:08X}", scope_word(i, 8));
        continue;
      }
      const int32_t result = int32_t(ctx->r[3]);
      if (result < 0) {
        finish(kExceptionContinueExecution);
        return;
      }
      if (result > 0) {
        // The unwind starts at this handler's caller.
        restore();
        UnwindGuestFrames(ctx, establisher, scope_word(i, 0xC),
                          record.guest_address(), record->code,
                          context.guest_address());
        return;
      }
    }
    finish(kExceptionContinueSearch);
    return;
  }

  // An unwind stops at the scope of its target and runs the finally handlers
  // it passes on the way.
  const uint32_t target_pc =
      xe::load_and_swap<uint32_t>(context.as<uint8_t*>() + kContextIar);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t begin = scope_word(i, 0);
    const uint32_t end = scope_word(i, 4);
    if (control_pc < begin || control_pc >= end) {
      continue;
    }
    if (target_pc >= begin && target_pc <= end) {
      break;
    }
    if (const uint32_t target = scope_word(i, 0xC)) {
      if (target_pc == target) {
        break;
      }
      continue;
    }
    xe::store_and_swap<uint32_t>(dispatcher + 0x0, end + 4);
    ctx->r[3] = 1;
    ctx->r[12] = establisher;
    if (!CallGuestFromKernel(ctx, scope_word(i, 8), frame)) {
      XELOGE("__C_specific_handler: no finally handler at {:08X}",
             scope_word(i, 8));
    }
  }
  finish(kExceptionContinueSearch);
}
DECLARE_XBOXKRNL_EXPORT1(__C_specific_handler, kNone, kImplemented);

void RtlRaiseException_entry(pointer_t<X_EXCEPTION_RECORD> record,
                             const ppc_context_t& context) {
  switch (record->code) {
    case 0x406D1388: {
      HandleSetThreadName(record);
      return;
    }
    case 0xE06D7363: {
      HandleCppException(record, context);
      return;
    }
  }

  // TODO(benvanik): unwinding.
  // This is going to suck.
  // xe::debugging::Break();

  // RtlRaiseException definitely wasn't a noreturn function, we can return
  // safe-ish
  XELOGE("Guest attempted to trigger a breakpoint!");
}
DECLARE_XBOXKRNL_EXPORT1(RtlRaiseException, kDebug, kStub);

void KeBugCheckEx_entry(dword_t code, dword_t param1, dword_t param2,
                        dword_t param3, dword_t param4) {
  auto msg =
      fmt::format("*** STOP: 0x{:08X} (0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X})",
                  static_cast<uint32_t>(code), static_cast<uint32_t>(param1),
                  static_cast<uint32_t>(param2), static_cast<uint32_t>(param3),
                  static_cast<uint32_t>(param4));
  XELOGE("{}", msg);
  fflush(stdout);

  if (xe::debugging::IsDebuggerAttached()) {
    xe::debugging::Break();
  }

  // Show crash dialog and suspend the guest thread instead of killing the
  // host process.
  auto current_thread = kernel::XThread::GetCurrentThread();
  const auto* emulator = kernel_state()->emulator();
  auto* display_window = emulator->display_window();
  auto* imgui_drawer = emulator->imgui_drawer();
  if (display_window && imgui_drawer) {
    auto dlg_msg = fmt::format(
        "The guest kernel has crashed (KeBugCheck).\n\n{}\n\n"
        "The faulting thread has been suspended.",
        msg);
    display_window->app_context().CallInUIThreadSynchronous(
        [imgui_drawer, &dlg_msg]() {
          xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer,
                                              "Guest Kernel Crash", dlg_msg);
        });
  }

  if (current_thread) {
    current_thread->Suspend(nullptr);
  }
}
DECLARE_XBOXKRNL_EXPORT2(KeBugCheckEx, kDebug, kStub, kImportant);

void KeBugCheck_entry(dword_t code) { KeBugCheckEx_entry(code, 0, 0, 0, 0); }
DECLARE_XBOXKRNL_EXPORT2(KeBugCheck, kDebug, kImplemented, kImportant);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Debug);
