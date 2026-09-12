/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/cpu_flags.h"

DEFINE_string(cpu, "any", "Does nothing. CPU backend [any, x64].", "CPU");

DEFINE_string(
    load_module_map, "",
    "Loads a .map for symbol names and to diff with the generated symbol "
    "database.",
    "CPU");

DEFINE_bool(accurate_vmx_denormal_flush, false,
            "Flush denormals in the VMX multiply-add and dot product opcodes "
            "even while the guest has NJM cleared, which is what hardware "
            "does. Costs performance in vector heavy code and only matters to "
            "a title that clears VSCR.NJ through mtvscr.",
            "CPU");

DEFINE_bool(disassemble_functions, false,
            "Disassemble functions during generation.", "CPU");

DEFINE_bool(trace_function_coverage, false,
            "Count how many times each guest instruction executes and report "
            "the totals in the guestcoverage section of the profiler dump.",
            "CPU");

DEFINE_uint32(
    cpu_trace_mask, 0,
    "JIT execution trace modes to log (bitmask): 1=instructions, 2=data, "
    "4=function calls (7=all). Each mode must be compiled in to be usable.",
    "CPU");

DEFINE_bool(validate_hir, false,
            "Perform validation checks on the HIR during compilation.", "CPU");

// https://github.com/bitsh1ft3r/Xenon/blob/091e8cd4dc4a7c697b4979eb200be7c9dee3590b/Xenon/Core/XCPU/PPU/PowerPC.h#L370
DEFINE_uint64(
    pvr, 0x710700,
    "Known PVR's.\n"
    " 0x710200 = Used by Zephyr \n"
    " 0x710300 = Used by Zephyr\n"
    " 0x710500 = Used by Jasper\n"
    " 0x710700 = Default\n"
    " 0x710800 = Used by Corona V1 & V2\n"
    "Processor version and revision number.\nBits 0 to 15 are the version "
    "number.\nBits 16 to 31 are the revision number.\nNote: Some XEXs (such as "
    "mfgbootlauncher.xex) may check for a value that's less than 0x710700.",
    "CPU");

// Breakpoints:
DEFINE_uint64(break_on_instruction, 0,
              "int3 before the given guest address is executed.", "CPU");
DEFINE_int32(break_condition_gpr, -1, "GPR compared to", "CPU");
DEFINE_uint64(break_condition_value, 0, "value compared against", "CPU");
DEFINE_string(break_condition_op, "eq", "comparison operator", "CPU");
DEFINE_bool(break_condition_truncate, true, "truncate value to 32-bits", "CPU");

DEFINE_bool(break_on_debugbreak, true, "int3 on JITed __debugbreak requests.",
            "CPU");

DEFINE_uint32(db16cyc_yield_after, 2,
              "Consecutive guest db16cyc delays before the thread sleeps "
              "instead of spinning; 0 disables.",
              "CPU");

DEFINE_uint32(spin_wait_yield_after, 100000,
              "Iterations of a spin-shaped guest loop before the thread "
              "sleeps; 0 disables the pass.",
              "CPU");

DEFINE_uint32(spin_wait_max_iter_ns, 1000,
              "Longest average iteration, in ns, that still counts as a wait.",
              "CPU");
