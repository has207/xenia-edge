test_mtfsfix_1:
  mtfsfi. 0, 0
  blr

test_mtfsfix_2:
  mtfsfi. 1, 1
  blr

test_mtfsfix_3:
  mtfsfi. 7, 15
  blr

test_mtfsfix_4:
  mtfsfi. 0, 7
  blr

# The immediate is the top 4 bits of the RB field. NI always reads back 0.
test_mtfsfix_5:
  #_ REGISTER_IN f1 0x0000000000000000
  mtfsf 0xFF, f1
  mtfsfi 7, 3
  mffs f2
  mtfsfi 7, 15
  mffs f3
  mtfsfi 7, 0
  blr
  #_ REGISTER_OUT f1 0x0000000000000000
  #_ REGISTER_OUT f2 0xFFF8000000000003
  #_ REGISTER_OUT f3 0xFFF800000000000B
