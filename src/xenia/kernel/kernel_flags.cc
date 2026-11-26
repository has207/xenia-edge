/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/kernel_flags.h"

DEFINE_bool(log_high_frequency_kernel_calls, false,
            "Log kernel calls with the kHighFrequency tag.", "Logging");
DEFINE_uint32(
    guest_scheduler_quantum_us, 1000,
    "Cooperative-scheduler timeslice in microseconds. A guest fiber running "
    "this long yields at its next JIT safepoint so co-resident fibers on the "
    "same dispatch thread make progress. Lower is fairer but switches more.",
    "Kernel");
