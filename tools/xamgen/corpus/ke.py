"""Ke corpus: the header KeInitializeMutant leaves in a KMUTANT in the arena,
the previous state KeReleaseMutant returns for a mutant held once and twice,
and whether KeSetTimer and KeCancelTimer report a timer as already set.

Only a mutant's first 8 bytes (type, flags and signal state) are captured,
since the rest holds kernel list links and the owner's KTHREAD. Each mutant
has a block of its own so none is initialized again while a thread owns it.
Every acquire is released. The timer is cancelled before the end so the kernel
keeps no link into the title's memory. Its due time is short enough that it
would still expire on its own after a fault.
"""

MUTANT_SIZE = 0x20
TIMER_SIZE = 0x28
EXECUTIVE = 0
KERNEL_MODE = 0
NOTIFICATION = 0
TITLE_PROCESS = 1
DUE_MS = 200


def acquire(s, mutant, name):
    timeout = s.alloc(8, 'zero timeout', fill=0)
    s.call('KeWaitForSingleObject', mutant, EXECUTIVE, KERNEL_MODE, 0,
           timeout, name=name, capture=[mutant.prefix(8)])


def release(s, mutant, name):
    s.call('KeReleaseMutant', mutant, 0, 0, 0, name=name,
           capture=[mutant.prefix(8)])


def build(corpus):
    s = corpus.scenario('mutant owned from the start')
    mutant = s.alloc(MUTANT_SIZE, 'mutant', fill=0)
    s.call('KeInitializeMutant', mutant, 1, void=True,
           capture=[mutant.prefix(8)])
    acquire(s, mutant, 'acquire again')
    release(s, mutant, 'release inner')
    release(s, mutant, 'release outer')

    s = corpus.scenario('mutant free from the start')
    mutant = s.alloc(MUTANT_SIZE, 'mutant', fill=0)
    s.call('KeInitializeMutant', mutant, 0, void=True,
           capture=[mutant.prefix(8)])
    acquire(s, mutant, 'acquire')
    release(s, mutant, 'release')
    acquire(s, mutant, 'acquire after release')
    release(s, mutant, 'release again')

    s = corpus.scenario('timer')
    timer = s.alloc(TIMER_SIZE, 'timer', fill=0)
    due = -DUE_MS * 10000
    s.call('KeInitializeTimerEx', timer, NOTIFICATION, TITLE_PROCESS,
           void=True)
    s.call('KeSetTimer', timer, due, 0, name='set')
    s.call('KeSetTimer', timer, due, 0, name='set again')
    s.call('KeCancelTimer', timer, name='cancel')
    s.call('KeCancelTimer', timer, name='cancel again')
