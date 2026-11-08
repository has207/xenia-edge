test_mcrfs_1:
  #_ REGISTER_IN f1 1.0
  #_ REGISTER_IN f2 2.0
  fadds f3, f1, f2
  mcrfs 1, 0
  blr
  #_ REGISTER_OUT f1 1.0
  #_ REGISTER_OUT f2 2.0
  #_ REGISTER_OUT f3 3.0

test_mcrfs_2:
  #_ REGISTER_IN f1 0x7FF0000000000001
  #_ REGISTER_IN f2 1.0
  fadd f3, f1, f2
  mcrfs 2, 0
  blr
  #_ REGISTER_OUT f1 0x7FF0000000000001
  #_ REGISTER_OUT f2 1.0
  #_ REGISTER_OUT f3 0x7FF8000000000001

# The control field has no exception bits, so the rounding mode stays.
test_mcrfs_3:
  #_ REGISTER_IN f1 0x0000000000000001
  #_ REGISTER_IN cr 0x00000000
  mtfsf 0xFF, f1
  mcrfs 7, 7
  mffs f2
  mtfsb0 31
  blr
  #_ REGISTER_OUT f2 0xFFF8000000000001
  #_ REGISTER_OUT cr 0x00000001

# UX, ZX and XX are cleared, FR and FI stay.
test_mcrfs_4:
  #_ REGISTER_IN f1 0x000000000E060000
  #_ REGISTER_IN cr 0x00000000
  mtfsf 0xFF, f1
  mcrfs 6, 1
  mcrfs 5, 3
  mffs f2
  blr
  #_ REGISTER_OUT f2 0xFFF8000000060000
  #_ REGISTER_OUT cr 0x000006E0

# Clearing the last VX cause clears VX.
test_mcrfs_5:
  #_ REGISTER_IN f1 0x00000000A0000100
  #_ REGISTER_IN cr 0x00000000
  mtfsf 0xFF, f1
  mcrfs 2, 5
  mffs f2
  mtfsb0 0
  blr
  #_ REGISTER_OUT f2 0xFFF8000080000000
  #_ REGISTER_OUT cr 0x00100000

test_mcrfs_6:
  #_ REGISTER_IN f1 0x00000000A1000000
  #_ REGISTER_IN cr 0x00000000
  mtfsf 0xFF, f1
  mcrfs 2, 1
  mffs f2
  mtfsb0 0
  blr
  #_ REGISTER_OUT f2 0xFFF8000080000000
  #_ REGISTER_OUT cr 0x00100000

# Clearing the enabled exception clears FEX.
test_mcrfs_7:
  #_ REGISTER_IN f1 0x0000000002000008
  #_ REGISTER_IN cr 0x00000000
  mtfsf 0xFF, f1
  mcrfs 2, 1
  mffs f2
  mtfsb0 28
  blr
  #_ REGISTER_OUT f2 0xFFF8000000000008
  #_ REGISTER_OUT cr 0x00200000
