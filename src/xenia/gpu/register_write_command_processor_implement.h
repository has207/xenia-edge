/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Register range writes shared by the D3D12 and Vulkan command processors. A
// run of registers is split where the ranges a backend tracks begin and end.
// Each piece is byte-swapped into the register file in one copy.
//
// Included into each backend's source file inside its namespace, with
// COMMAND_PROCESSOR defined as the class and
// register_write_command_processor_declare.h included in the class body. The
// backend defines the four functions that header lists last. No include guard.

void COMMAND_PROCESSOR::WriteRegistersFromMem(uint32_t start_index,
                                              uint32_t* base,
                                              uint32_t num_registers) {
  WriteRegisterRangeFromMem_WithKnownBound<0, 0xFFFF>(start_index, base,
                                                      num_registers);
}

void COMMAND_PROCESSOR::WriteALURangeFromRing(xe::RingBuffer* ring,
                                              uint32_t base,
                                              uint32_t num_times) {
  WriteRegisterRangeFromRing_WithKnownBound<
      XE_GPU_REG_SHADER_CONSTANT_000_X, XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0>(
      ring, base + XE_GPU_REG_SHADER_CONSTANT_000_X, num_times);
}

void COMMAND_PROCESSOR::WriteFetchRangeFromRing(xe::RingBuffer* ring,
                                                uint32_t base,
                                                uint32_t num_times) {
  WriteRegisterRangeFromRing_WithKnownBound<0x4800, 0x5002>(ring, base + 0x4800,
                                                            num_times);
}

XE_FORCEINLINE
void COMMAND_PROCESSOR::WriteBoolRangeFromRing(xe::RingBuffer* ring,
                                               uint32_t base,
                                               uint32_t num_times) {
  WriteRegisterRangeFromRing_WithKnownBound<0x4900, 0x5002>(ring, base + 0x4900,
                                                            num_times);
}

XE_FORCEINLINE
void COMMAND_PROCESSOR::WriteLoopRangeFromRing(xe::RingBuffer* ring,
                                               uint32_t base,
                                               uint32_t num_times) {
  WriteRegisterRangeFromRing_WithKnownBound<0x4908, 0x5002>(ring, base + 0x4908,
                                                            num_times);
}

XE_FORCEINLINE
void COMMAND_PROCESSOR::WriteREGISTERSRangeFromRing(xe::RingBuffer* ring,
                                                    uint32_t base,
                                                    uint32_t num_times) {
  WriteRegisterRangeFromRing_WithKnownBound<0x2000, 0x2000 + 0x800>(
      ring, base + 0x2000, num_times);
}

XE_FORCEINLINE
void COMMAND_PROCESSOR::WriteALURangeFromMem(uint32_t start_index,
                                             uint32_t* base,
                                             uint32_t num_registers) {
  WriteRegisterRangeFromMem_WithKnownBound<
      XE_GPU_REG_SHADER_CONSTANT_000_X, XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0>(
      start_index + 0x4000, base, num_registers);
}

XE_FORCEINLINE
void COMMAND_PROCESSOR::WriteFetchRangeFromMem(uint32_t start_index,
                                               uint32_t* base,
                                               uint32_t num_registers) {
  WriteRegisterRangeFromMem_WithKnownBound<0x4800, 0x5002>(start_index + 0x4800,
                                                           base, num_registers);
}

XE_FORCEINLINE
void COMMAND_PROCESSOR::WriteBoolRangeFromMem(uint32_t start_index,
                                              uint32_t* base,
                                              uint32_t num_registers) {
  WriteRegisterRangeFromMem_WithKnownBound<0x4900, 0x5002>(start_index + 0x4900,
                                                           base, num_registers);
}

XE_FORCEINLINE
void COMMAND_PROCESSOR::WriteLoopRangeFromMem(uint32_t start_index,
                                              uint32_t* base,
                                              uint32_t num_registers) {
  WriteRegisterRangeFromMem_WithKnownBound<0x4908, 0x5002>(start_index + 0x4908,
                                                           base, num_registers);
}

XE_FORCEINLINE
void COMMAND_PROCESSOR::WriteREGISTERSRangeFromMem(uint32_t start_index,
                                                   uint32_t* base,
                                                   uint32_t num_registers) {
  WriteRegisterRangeFromMem_WithKnownBound<0x2000, 0x2000 + 0x800>(
      start_index + 0x2000, base, num_registers);
}
/*
wraparound rarely happens, so its best to hoist this out of
writeregisterrangefromring, and structure the two functions so that this can be
tail called
*/
XE_NOINLINE
void COMMAND_PROCESSOR::WriteRegisterRangeFromRing_WraparoundCase(
    xe::RingBuffer* ring, uint32_t base, uint32_t num_registers) {
  // we already brought it into L2 earlier
  RingBuffer::ReadRange range =
      ring->BeginPrefetchedRead<swcache::PrefetchTag::Level1>(num_registers *
                                                              sizeof(uint32_t));

  uint32_t num_regs_firstrange =
      static_cast<uint32_t>(range.first_length / sizeof(uint32_t));

  COMMAND_PROCESSOR::WriteRegistersFromMem(
      base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
      num_regs_firstrange);

  COMMAND_PROCESSOR::WriteRegistersFromMem(
      base + num_regs_firstrange,
      reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.second)),
      num_registers - num_regs_firstrange);

  ring->EndRead(range);
}
void COMMAND_PROCESSOR::WriteRegisterRangeFromRing(xe::RingBuffer* ring,
                                                   uint32_t base,
                                                   uint32_t num_registers) {
  RingBuffer::ReadRange range =
      ring->BeginRead(num_registers * sizeof(uint32_t));

  XE_LIKELY_IF(!range.second) {
    WriteRegistersFromMem(
        base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
        num_registers);

    ring->EndRead(range);
  }
  else {
    return WriteRegisterRangeFromRing_WraparoundCase(ring, base, num_registers);
  }
}

void COMMAND_PROCESSOR::WritePossiblySpecialRegistersFromMem(
    uint32_t start_index, uint32_t* base, uint32_t numregs) {
  uint32_t end = numregs + start_index;
  for (uint32_t index = start_index; index < end; ++index, ++base) {
    uint32_t value = xe::load_and_swap<uint32_t>(base);

    register_file_->values[index] = value;

    unsigned expr = 0;

    expr |= (index - XE_GPU_REG_SCRATCH_REG0 < 8);

    expr |= (index == XE_GPU_REG_COHER_STATUS_HOST);

    expr |= ((index - XE_GPU_REG_DC_LUT_RW_INDEX) <=
             (XE_GPU_REG_DC_LUT_30_COLOR - XE_GPU_REG_DC_LUT_RW_INDEX));

    if (expr == 0) {
    } else {
      HandleSpecialRegisterWrite(index, value);
    }
  }
}
template <uint32_t register_lower_bound, uint32_t register_upper_bound>
XE_FORCEINLINE void COMMAND_PROCESSOR::WriteRegisterRangeFromMem_WithKnownBound(
    uint32_t start_index, uint32_t* base, uint32_t num_registers) {
  LogRegisterSets(start_index, base, num_registers);
  // Registers past the end of the register file are dropped.
  uint32_t end = std::min(start_index + num_registers,
                          uint32_t(RegisterFile::kRegisterCount));
  if (start_index >= end) {
    return;
  }
  uint32_t current_index = start_index;

  auto get_end_before_qty = [&end, &current_index](uint32_t regnum) {
    return std::min<uint32_t>(regnum, end) - current_index;
  };
#define REGULAR_WRITE_CALLBACK(s, e, i, b, n) \
  WriteRegularRegistersFromMem(i, b, n)
#define WRITE_FETCH_CONSTANTS_CALLBACK(str, er, ind, b, n) \
  WriteFetchFromMem(ind, b, n)
#define SPECIAL_REG_RANGE_CALLBACK(str, edr, ind, bs, n) \
  WritePossiblySpecialRegistersFromMem(ind, bs, n)
#define WRITE_SHADER_CONSTANTS_CALLBACK(start_range, end_range, index, base, \
                                        n)                                   \
  WriteShaderConstantsFromMem(index, base, n)
#define WRITE_BOOL_LOOP_CALLBACK(s, e, i, b, n) WriteBoolLoopFromMem(i, b, n)

#define DO_A_RANGE(start_range, end_range, cb)                       \
  if constexpr (start_range >= register_lower_bound ||               \
                end_range > register_lower_bound) {                  \
    if (current_index < (end_range)) {                               \
      uint32_t ntowrite = get_end_before_qty(end_range);             \
      cb((start_range), (end_range), current_index, base, ntowrite); \
      current_index += ntowrite;                                     \
      base += ntowrite;                                              \
    }                                                                \
    if (current_index >= end) {                                      \
      return;                                                        \
    }                                                                \
  }

#define REFRESH_MSVC_RANGE()                              \
  XE_MSVC_ASSUME(current_index >= register_lower_bound && \
                 current_index < register_upper_bound)

  REFRESH_MSVC_RANGE();

  DO_A_RANGE(0, XE_GPU_REG_SCRATCH_REG0, REGULAR_WRITE_CALLBACK);
  REFRESH_MSVC_RANGE();
  DO_A_RANGE(XE_GPU_REG_SCRATCH_REG0, XE_GPU_REG_DC_LUT_30_COLOR + 1,
             SPECIAL_REG_RANGE_CALLBACK);
  REFRESH_MSVC_RANGE();
  DO_A_RANGE(XE_GPU_REG_DC_LUT_30_COLOR + 1, XE_GPU_REG_SHADER_CONSTANT_000_X,
             REGULAR_WRITE_CALLBACK);
  REFRESH_MSVC_RANGE();
  DO_A_RANGE(XE_GPU_REG_SHADER_CONSTANT_000_X,
             XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
             WRITE_SHADER_CONSTANTS_CALLBACK);
  REFRESH_MSVC_RANGE();
  DO_A_RANGE(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
             XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 + 1,
             WRITE_FETCH_CONSTANTS_CALLBACK);
  REFRESH_MSVC_RANGE();
  DO_A_RANGE(XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031,
             XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1, WRITE_BOOL_LOOP_CALLBACK);
  REFRESH_MSVC_RANGE();
  DO_A_RANGE(XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1, 65536,
             REGULAR_WRITE_CALLBACK);

#undef REFRESH_MSVC_RANGE
#undef DO_A_RANGE
#undef WRITE_BOOL_LOOP_CALLBACK
#undef WRITE_SHADER_CONSTANTS_CALLBACK
#undef SPECIAL_REG_RANGE_CALLBACK
#undef WRITE_FETCH_CONSTANTS_CALLBACK
#undef REGULAR_WRITE_CALLBACK
}
template <uint32_t register_lower_bound, uint32_t register_upper_bound>
XE_FORCEINLINE void
COMMAND_PROCESSOR::WriteRegisterRangeFromRing_WithKnownBound(
    xe::RingBuffer* ring, uint32_t base, uint32_t num_registers) {
  RingBuffer::ReadRange range =
      ring->BeginRead(num_registers * sizeof(uint32_t));

  XE_LIKELY_IF(!range.second) {
    WriteRegisterRangeFromMem_WithKnownBound<register_lower_bound,
                                             register_upper_bound>(
        base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
        num_registers);

    ring->EndRead(range);
  }
  else {
    return WriteRegisterRangeFromRing_WraparoundCase(ring, base, num_registers);
  }
}
XE_NOINLINE
void COMMAND_PROCESSOR::WriteOneRegisterFromRing(uint32_t base,
                                                 uint32_t num_times) {
  auto read = reader_.BeginPrefetchedRead<swcache::PrefetchTag::Level1>(
      num_times * sizeof(uint32_t));

  uint32_t first_length = read.first_length / sizeof(uint32_t);

  for (uint32_t i = 0; i < first_length; ++i) {
    COMMAND_PROCESSOR::WriteRegister(
        base, xe::load_and_swap<uint32_t>(read.first + (sizeof(uint32_t) * i)));
  }

  XE_UNLIKELY_IF(read.second) {
    uint32_t second_length = read.second_length / sizeof(uint32_t);

    for (uint32_t i = 0; i < second_length; ++i) {
      COMMAND_PROCESSOR::WriteRegister(
          base,
          xe::load_and_swap<uint32_t>(read.second + (sizeof(uint32_t) * i)));
    }
  }
  reader_.EndRead(read);
}
