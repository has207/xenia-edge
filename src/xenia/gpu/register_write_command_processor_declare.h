/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Declarations for register_write_command_processor_implement.h, included into
// the class body of each command processor that uses it. No include guard.

// Whole-file and range formatting disagree on the indent of a class-body
// fragment.
// clang-format off
virtual void WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                   uint32_t num_registers) override;

void WritePossiblySpecialRegistersFromMem(uint32_t start_index, uint32_t* base,
                                          uint32_t num_registers);
template <uint32_t register_lower_bound, uint32_t register_upper_bound>
XE_FORCEINLINE void WriteRegisterRangeFromMem_WithKnownBound(
    uint32_t start_index, uint32_t* base, uint32_t num_registers);
XE_FORCEINLINE
virtual void WriteRegisterRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                                        uint32_t num_registers) override;
template <uint32_t register_lower_bound, uint32_t register_upper_bound>
XE_FORCEINLINE void WriteRegisterRangeFromRing_WithKnownBound(
    xe::RingBuffer* ring, uint32_t base, uint32_t num_registers);

XE_NOINLINE
void WriteRegisterRangeFromRing_WraparoundCase(xe::RingBuffer* ring,
                                               uint32_t base,
                                               uint32_t num_registers);
XE_NOINLINE
void WriteOneRegisterFromRing(uint32_t base, uint32_t num_times);

XE_FORCEINLINE
void WriteALURangeFromRing(xe::RingBuffer* ring, uint32_t base,
                           uint32_t num_times);

XE_FORCEINLINE
void WriteFetchRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                             uint32_t num_times);

XE_FORCEINLINE
void WriteBoolRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                            uint32_t num_times);

XE_FORCEINLINE
void WriteLoopRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                            uint32_t num_times);

XE_FORCEINLINE
void WriteREGISTERSRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                                 uint32_t num_times);

XE_FORCEINLINE
void WriteALURangeFromMem(uint32_t start_index, uint32_t* base,
                          uint32_t num_registers);

XE_FORCEINLINE
void WriteFetchRangeFromMem(uint32_t start_index, uint32_t* base,
                            uint32_t num_registers);

XE_FORCEINLINE
void WriteBoolRangeFromMem(uint32_t start_index, uint32_t* base,
                           uint32_t num_registers);

XE_FORCEINLINE
void WriteLoopRangeFromMem(uint32_t start_index, uint32_t* base,
                           uint32_t num_registers);

XE_FORCEINLINE
void WriteREGISTERSRangeFromMem(uint32_t start_index, uint32_t* base,
                                uint32_t num_registers);

// Defined by the backend. Each records what a write to its range makes
// outdated and copies the big-endian values into the register file. A range
// never crosses into another one and is never empty.
// Float constants.
XE_FORCEINLINE
void WriteShaderConstantsFromMem(uint32_t start_index, uint32_t* base,
                                 uint32_t num_registers);
// Bool and loop constants.
XE_FORCEINLINE
void WriteBoolLoopFromMem(uint32_t start_index, uint32_t* base,
                          uint32_t num_registers);
// Fetch constants.
XE_FORCEINLINE
void WriteFetchFromMem(uint32_t start_index, uint32_t* base,
                       uint32_t num_registers);
// Everything else that has no special write handling in the base class.
XE_FORCEINLINE
void WriteRegularRegistersFromMem(uint32_t start_index, uint32_t* base,
                                  uint32_t num_registers);
// clang-format on
