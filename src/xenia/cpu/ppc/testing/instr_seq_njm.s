# VSCR.NJ (non-Java mode) regression: NJ=1 (the 360 default) flushes vector
# FP denormal inputs to signed zero; NJ=0 preserves them. mtvscr's extract
# reads NJ from bit 16 of word 3 of VB. Exercises SET_NJM's mode-variant
# switching on both backends, which no other suite covers.
test_njm_off_preserves_denormals:
  #_ REGISTER_IN v4 [00000001, 00000001, 00000001, 00000001]
  #_ REGISTER_IN v5 [00000000, 00000000, 00000000, 00000000]
  #_ REGISTER_IN v6 [00000000, 00000000, 00000000, 00000000]
  mtvscr v6
  vaddfp v7, v4, v5
  blr
  #_ REGISTER_OUT v7 [00000001, 00000001, 00000001, 00000001]

test_njm_on_flushes_denormals:
  #_ REGISTER_IN v4 [00000001, 00000001, 00000001, 00000001]
  #_ REGISTER_IN v5 [00000000, 00000000, 00000000, 00000000]
  #_ REGISTER_IN v6 [00000000, 00000000, 00000000, 00010000]
  mtvscr v6
  vaddfp v7, v4, v5
  blr
  #_ REGISTER_OUT v7 [00000000, 00000000, 00000000, 00000000]

test_njm_flip_and_back:
  #_ REGISTER_IN v4 [80000001, 80000001, 80000001, 80000001]
  #_ REGISTER_IN v5 [00000000, 00000000, 00000000, 00000000]
  #_ REGISTER_IN v6 [00000000, 00000000, 00000000, 00000000]
  #_ REGISTER_IN v8 [00000000, 00000000, 00000000, 00010000]
  mtvscr v6
  vaddfp v7, v4, v5
  mtvscr v8
  vaddfp v9, v4, v5
  blr
  #_ REGISTER_OUT v7 [80000001, 80000001, 80000001, 80000001]
  # NJ=1 flushes the -denormal input to -0, and IEEE (-0) + (+0) = +0.
  #_ REGISTER_OUT v9 [00000000, 00000000, 00000000, 00000000]
