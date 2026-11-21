"""APC corpus: what the routines of an APC queued to the executor's own thread
are called with, and when they run, and an alertable wait on a signalled
event with a user APC queued.

Every scenario removes its APC at the end, since the next scenario lays its
arena over it.
"""

import struct

KERNEL_MODE = 0
USER_MODE = 1
NOTIFICATION = 0
CURRENT_THREAD = 0xFFFFFFFE
KAPC_SIZE = 0x28
CONTEXT = 0x3333
ARG1 = 0x1111
ARG2 = 0x2222


def interval(s, milliseconds):
    return s.alloc(8, 'interval', struct.pack('>q', -milliseconds * 10000))


def current_thread(s):
    thread = s.variable('thread')
    out = s.alloc(4, 'thread')
    s.call('ObReferenceObjectByHandle', CURRENT_THREAD, 0, out,
           store=[(thread, out)])
    return thread


def build(corpus):
    s = corpus.scenario('user apc')
    thread = current_thread(s)
    apc = s.alloc(KAPC_SIZE, 'apc')
    kernel = s.recorder('kernel routine')
    normal = s.recorder('normal routine', args=3)
    s.call('KeInitializeApc', apc, thread, kernel, 0, normal, USER_MODE,
           CONTEXT, void=True)
    s.call('KeInsertQueueApc', apc, ARG1, ARG2, 0,
           capture=[kernel.log, normal.log])
    s.call('KeDelayExecutionThread', USER_MODE, 1, interval(s, 10),
           name='alertable wait', capture=[kernel.log, normal.log])
    s.call('KeRemoveQueueApc', apc)
    s.call('ObDereferenceObject', thread, void=True)

    # No normal routine, so the kernel routine runs as soon as it can.
    s = corpus.scenario('kernel apc')
    thread = current_thread(s)
    apc = s.alloc(KAPC_SIZE, 'apc')
    kernel = s.recorder('kernel routine')
    s.call('KeInitializeApc', apc, thread, kernel, 0, 0, KERNEL_MODE, 0,
           void=True)
    s.call('KeInsertQueueApc', apc, ARG1, ARG2, 0, capture=[kernel.log])
    s.call('KeDelayExecutionThread', KERNEL_MODE, 0, interval(s, 10),
           name='wait', capture=[kernel.log])
    s.call('KeRemoveQueueApc', apc)
    s.call('ObDereferenceObject', thread, void=True)

    s = corpus.scenario('NtQueueApcThread')
    normal = s.recorder('normal routine', args=3)
    s.call('NtQueueApcThread', CURRENT_THREAD, normal, CONTEXT, ARG1, ARG2,
           capture=[normal.log])
    s.call('KeDelayExecutionThread', USER_MODE, 1, interval(s, 10),
           name='alertable wait', capture=[normal.log])

    # The signalled event satisfies the alertable wait, which leaves the APC
    # queued for the next alertable wait to run.
    s = corpus.scenario('alertable wait on a signalled event')
    normal = s.recorder('normal routine', args=3)
    event_slot = s.alloc(4, 'event')
    event = s.variable('event')
    s.call('NtCreateEvent', event_slot, 0, NOTIFICATION, 1,
           store=[(event, event_slot)])
    s.call('NtQueueApcThread', CURRENT_THREAD, normal, CONTEXT, ARG1, ARG2,
           capture=[normal.log])
    s.call('NtWaitForSingleObjectEx', event, USER_MODE, 1, interval(s, 10),
           name='alertable wait', capture=[normal.log])
    s.call('KeDelayExecutionThread', USER_MODE, 1, interval(s, 10),
           name='alertable delay', capture=[normal.log])
    s.call('NtClose', event)
