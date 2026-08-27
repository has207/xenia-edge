# Longjmp repair regression: a guest control transfer that skips two live
# frames must be caught by the resolve-time stackpoint walk and repaired by
# the sync helper. Shape: T0 -> T -> W -> E -> L; T saves LR/r1 setjmp-style
# BEFORE its stwu (so the walk breaks at T's own node, where both backends'
# SP-restore arithmetic provably coincides), and L longjmps to T's return
# site. L's blr tail-pops its own node, leaving E and W skipped (= 2 > 1).
test_stacksync_longjmp_repair:
  #_ REGISTER_IN r1 0x1000F000
  #_ REGISTER_IN r20 0
  #_ REGISTER_IN r21 0
  mfspr r12, lr
  stwu r1, -0x60(r1)
  bl lj_t
lj_t0_ret:
  addi r21, r21, 100
  addi r1, r1, 0x60
  mtspr lr, r12
  blr
lj_t:
  mfspr r13, lr
  bl lj_t_anchor
lj_t_anchor:
  mfspr r17, lr
  addi r17, r17, 20
  or r16, r1, r1
  stwu r1, -0x40(r1)
  bl lj_w
lj_t_ret:
  addi r20, r20, 0x41
  mtspr lr, r13
  blr
lj_w:
  addi r21, r21, 1
  stwu r1, -0x30(r1)
  bl lj_e
  addi r1, r1, 0x30
  blr
lj_e:
  addi r21, r21, 1
  stwu r1, -0x30(r1)
  bl lj_l
  addi r1, r1, 0x30
  blr
lj_l:
  addi r21, r21, 1
  mtspr lr, r17
  or r1, r16, r16
  blr
  #_ REGISTER_OUT r1 0x1000F000
  #_ REGISTER_OUT r20 0x41
  #_ REGISTER_OUT r21 103
