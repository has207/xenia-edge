/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_CPU_FLAGS_H_
#define XENIA_CPU_CPU_FLAGS_H_
#include "xenia/base/cvar.h"

DECLARE_string(cpu);

DECLARE_string(load_module_map);

DECLARE_bool(accurate_vmx_denormal_flush);

DECLARE_bool(disassemble_functions);

DECLARE_bool(trace_function_coverage);

DECLARE_uint32(cpu_trace_mask);

DECLARE_bool(validate_hir);

DECLARE_uint64(pvr);

// Breakpoints:
DECLARE_uint64(break_on_instruction);
DECLARE_int32(break_condition_gpr);
DECLARE_uint64(break_condition_value);
DECLARE_string(break_condition_op);
DECLARE_bool(break_condition_truncate);

DECLARE_bool(break_on_debugbreak);

DECLARE_uint32(db16cyc_yield_after);
DECLARE_uint32(spin_wait_yield_after);
DECLARE_uint32(spin_wait_max_iter_ns);

#endif  // XENIA_CPU_CPU_FLAGS_H_
