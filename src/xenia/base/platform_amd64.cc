/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <string>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#define XBYAK_NO_OP_NAMES
#include "third_party/xbyak/xbyak/xbyak.h"
#include "third_party/xbyak/xbyak/xbyak_util.h"
DEFINE_int64(x64_extension_mask, -1LL,
             "Bitmask of the instruction set features the x64 recompiler may "
             "use. A feature is used only if the CPU reports it and its bit is "
             "set here, so clearing a bit forces the fallback path.\n"
             "        1 = AVX2\n"
             "        2 = FMA\n"
             "        4 = LZCNT (ABM, includes POPCNT)\n"
             "        8 = BMI1\n"
             "       16 = BMI2\n"
             "       32 = PREFETCHW\n"
             "       64 = MOVBE\n"
             "      128 = GFNI\n"
             "      256 = AVX512F\n"
             "      512 = AVX512VL\n"
             "     1024 = AVX512BW\n"
             "     2048 = AVX512DQ\n"
             "     4096 = AVX512VBMI\n"
             "     8192 = fast JRCXZ (AMD)\n"
             "    16384 = fast LOOP (AMD)\n"
             "    32768 = independent EFLAGS renaming (Zen and newer)\n"
             "    65536 = XOP\n"
             "   131072 = FMA4\n"
             "   262144 = TBM\n"
             "   524288 = MOVDIR64B\n"
             "  1048576 = fast REP MOVSB (ERMS)\n"
             "       -1 = use every feature the CPU reports\n"
             "Every AVX512 path also requires AVX512F and AVX512VL, so -257 "
             "turns all of them off at once. The features actually in use are "
             "written to the log at startup.\n",
             "x64");
namespace xe {
namespace amd64 {
static uint64_t g_feature_flags = 0U;
static bool g_did_initialize_feature_flags = false;
uint64_t GetFeatureFlags() {
  if (!g_did_initialize_feature_flags) {
    InitFeatureFlags();
  }
  return g_feature_flags;
}

struct X64FeatureName {
  uint64_t bit;
  const char* name;
};

// Names match the values documented on x64_extension_mask.
static constexpr X64FeatureName kX64FeatureNames[] = {
    {kX64EmitAVX2, "AVX2"},
    {kX64EmitFMA, "FMA"},
    {kX64EmitLZCNT, "LZCNT"},
    {kX64EmitBMI1, "BMI1"},
    {kX64EmitBMI2, "BMI2"},
    {kX64EmitPrefetchW, "PREFETCHW"},
    {kX64EmitMovbe, "MOVBE"},
    {kX64EmitGFNI, "GFNI"},
    {kX64EmitAVX512F, "AVX512F"},
    {kX64EmitAVX512VL, "AVX512VL"},
    {kX64EmitAVX512BW, "AVX512BW"},
    {kX64EmitAVX512DQ, "AVX512DQ"},
    {kX64EmitAVX512VBMI, "AVX512VBMI"},
    {kX64FastJrcx, "FastJrcx"},
    {kX64FastLoop, "FastLoop"},
    {kX64FlagsIndependentVars, "FlagsIndependentVars"},
    {kX64EmitXOP, "XOP"},
    {kX64EmitFMA4, "FMA4"},
    {kX64EmitTBM, "TBM"},
    {kX64EmitMovdir64M, "MOVDIR64B"},
    {kX64FastRepMovs, "FastRepMovs"},
};

static std::string FormatFeatureList(uint64_t flags) {
  std::string result;
  for (const X64FeatureName& feature : kX64FeatureNames) {
    if ((flags & feature.bit) == feature.bit) {
      if (!result.empty()) {
        result += ' ';
      }
      result += feature.name;
    }
  }
  return result.empty() ? "(none)" : result;
}

XE_COLD
static void LogFeatureFlags(uint64_t detected, uint64_t active) {
  uint64_t known = 0U;
  for (const X64FeatureName& feature : kX64FeatureNames) {
    known |= feature.bit;
  }
  XELOGI("X64 features: detected 0x{:X}, x64_extension_mask {}, active 0x{:X}",
         detected, cvars::x64_extension_mask, active);
  XELOGI("X64 features in use: {}", FormatFeatureList(active));
  uint64_t masked_off = detected & ~active;
  if (masked_off) {
    XELOGW("X64 features disabled by x64_extension_mask: {}",
           FormatFeatureList(masked_off));
  }
  XELOGI("X64 features absent from this CPU: {}",
         FormatFeatureList(known & ~detected));
}

XE_COLD
XE_NOINLINE
void InitFeatureFlags() {
  uint64_t detected_flags_ = 0U;
  {
    Xbyak::util::Cpu cpu_;
#define TEST_EMIT_FEATURE(emit, ext) \
  detected_flags_ |= (cpu_.has(ext) ? emit : 0);

    TEST_EMIT_FEATURE(kX64EmitAVX2, Xbyak::util::Cpu::tAVX2);
    TEST_EMIT_FEATURE(kX64EmitFMA, Xbyak::util::Cpu::tFMA);
    TEST_EMIT_FEATURE(kX64EmitLZCNT, Xbyak::util::Cpu::tLZCNT);
    TEST_EMIT_FEATURE(kX64EmitBMI1, Xbyak::util::Cpu::tBMI1);
    TEST_EMIT_FEATURE(kX64EmitBMI2, Xbyak::util::Cpu::tBMI2);
    TEST_EMIT_FEATURE(kX64EmitMovbe, Xbyak::util::Cpu::tMOVBE);
    TEST_EMIT_FEATURE(kX64EmitGFNI, Xbyak::util::Cpu::tGFNI);
    TEST_EMIT_FEATURE(kX64EmitAVX512F, Xbyak::util::Cpu::tAVX512F);
    TEST_EMIT_FEATURE(kX64EmitAVX512VL, Xbyak::util::Cpu::tAVX512VL);
    TEST_EMIT_FEATURE(kX64EmitAVX512BW, Xbyak::util::Cpu::tAVX512BW);
    TEST_EMIT_FEATURE(kX64EmitAVX512DQ, Xbyak::util::Cpu::tAVX512DQ);
    TEST_EMIT_FEATURE(kX64EmitAVX512VBMI, Xbyak::util::Cpu::tAVX512VBMI);
    TEST_EMIT_FEATURE(kX64EmitPrefetchW, Xbyak::util::Cpu::tPREFETCHW);
#undef TEST_EMIT_FEATURE
    /*
    fix for xbyak bug/omission, amd cpus are never checked for lzcnt. fixed in
    latest version of xbyak
  */
    unsigned int data[4];
    Xbyak::util::Cpu::getCpuid(0x80000001, data);
    unsigned amd_flags = data[2];
    // chrispy: do prefetchw manually, prefetchw is one of the features xbyak
    // ignores if you have an amd cpu.

    if (amd_flags & (1U << 8)) {
      detected_flags_ |= kX64EmitPrefetchW;
    }
    if (amd_flags & (1U << 5)) {
      detected_flags_ |= kX64EmitLZCNT;
    }
    // todo: although not reported by cpuid, zen 1 and zen+ also have fma4
    if (amd_flags & (1U << 16)) {
      detected_flags_ |= kX64EmitFMA4;
    }
    if (amd_flags & (1U << 21)) {
      detected_flags_ |= kX64EmitTBM;
    }
    if (amd_flags & (1U << 11)) {
      detected_flags_ |= kX64EmitXOP;
    }
    if (cpu_.has(Xbyak::util::Cpu::tAMD)) {
      bool is_zennish = cpu_.displayFamily >= 0x17;
      /*
                  chrispy: according to agner's tables, all amd architectures
         that we support (ones with avx) have the same timings for
         jrcxz/loop/loope/loopne as for other jmps
          */
      detected_flags_ |= kX64FastJrcx;
      detected_flags_ |= kX64FastLoop;
      if (is_zennish) {
        // ik that i heard somewhere that this is the case for zen, but i need
        // to verify. cant find my original source for that. todo: ask agner?
        detected_flags_ |= kX64FlagsIndependentVars;
      }
    }
#if XE_PLATFORM_MAC
    // Rosetta 2 on macOS Sequoia (15.0+) translates AVX/AVX2/BMI1/BMI2/FMA,
    // but doesn't always advertise them via CPUID, so Xbyak misses them. Our
    // bundle's LSMinimumSystemVersion is 15.0, and every Intel Mac that can
    // run Sequoia natively (Coffee Lake / Ice Lake / Skylake-W) has AVX2
    // anyway, so force the bits on here, still subject to the cvar mask.
    constexpr uint64_t kX64MacForceMask =
        kX64EmitAVX2 | kX64EmitFMA | kX64EmitBMI1 | kX64EmitBMI2;
    detected_flags_ |= kX64MacForceMask;
#endif
  }
  {
    unsigned int data[4];
    memset(data, 0, sizeof(data));
    // intel extended features
    Xbyak::util::Cpu::getCpuidEx(7, 0, data);
    if (data[2] & (1 << 28)) {
      detected_flags_ |= kX64EmitMovdir64M;
    }
    if (data[1] & (1 << 9)) {
      detected_flags_ |= kX64FastRepMovs;
    }
  }
  g_feature_flags =
      detected_flags_ & static_cast<uint64_t>(cvars::x64_extension_mask);
  g_did_initialize_feature_flags = true;
  LogFeatureFlags(detected_flags_, g_feature_flags);
}
}  // namespace amd64
}  // namespace xe
