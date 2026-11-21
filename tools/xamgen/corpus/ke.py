"""Ke corpus: the header KeInitializeMutant leaves in a KMUTANT in the arena,
the previous state KeReleaseMutant returns for a mutant held once and twice,
whether KeSetTimer and KeCancelTimer report a timer as already set, and when
a DPC queued below and at DISPATCH_LEVEL runs.

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
DPC_SIZE = 0x20
DISPATCH_LEVEL = 2
CONTEXT = 0x3333
ARG1 = 0x1111
ARG2 = 0x2222


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

    # Queued below DISPATCH_LEVEL, a DPC runs at once and is off the queue by
    # the time KeInsertQueueDpc returns. Queued at DISPATCH_LEVEL, it waits for
    # the IRQL to drop, and one removed before then never runs. Only exports a
    # caller at DISPATCH_LEVEL may call run while it's raised.
    s = corpus.scenario('dpc')
    dpc = s.alloc(DPC_SIZE, 'dpc', fill=0)
    routine = s.recorder('dpc routine', args=4, calls=3)
    old_irql = s.variable('old irql')
    s.call('KeInitializeDpc', dpc, routine, CONTEXT, void=True)
    s.call('KeInsertQueueDpc', dpc, ARG1, ARG2, name='insert',
           capture=[routine.log])
    s.call('KeRemoveQueueDpc', dpc, name='remove after it ran')
    s.call('KfRaiseIrql', DISPATCH_LEVEL, store=[(old_irql, 'r3')])
    s.call('KeInsertQueueDpc', dpc, ARG1, ARG2,
           name='insert at DISPATCH_LEVEL', capture=[routine.log])
    s.call('KfLowerIrql', old_irql, name='lower', void=True,
           capture=[routine.log])
    s.call('KfRaiseIrql', DISPATCH_LEVEL, store=[(old_irql, 'r3')])
    s.call('KeInsertQueueDpc', dpc, ARG1, ARG2, name='insert to remove',
           capture=[routine.log])
    s.call('KeRemoveQueueDpc', dpc, name='remove while queued')
    s.call('KfLowerIrql', old_irql, name='lower after the remove', void=True,
           capture=[routine.log])
