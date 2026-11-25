"""Thread corpus: where ExCreateThread starts a thread, what
KeSetAffinityThread makes of a mask of several CPUs, how KeSetBasePriorityThread
moves the priority, and the background processors a title starts with.

Each thread starts at common.probe, which records where it runs and its
KTHREAD. A thread created suspended is inspected first: its KTHREAD current
CPU after each mask, or its priority words after each increment, and runs
once resumed. The last scenario moves the executor to CPU 3 to create a
thread from there, then puts it back with the affinity KeSetAffinityThread
reported.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import (KPCR_CURRENT_CPU, KPCR_CURRENT_THREAD,  # noqa: E402
                    KTHREAD_WORDS, message, probe)

SUSPENDED = 0x1
SYSTEM_THREAD = 0x2
AFFINITY_CPU4 = 0x10000000
USER_MODE = 1
EXIT_TIMEOUT_MS = 1000
KTHREAD_CURRENT_CPU = 0xBF
MASKS = (0x10, 0x3C, 0x3F, 0x05)
# THREAD_PRIORITY_* from below normal to time critical and idle.
INCREMENTS = (-1, -2, 1, 2, 15, -15, 0)


def current_cpu(s):
    def emit(a, r, thunk):
        a.label(r.entry)
        a.lbz(3, KPCR_CURRENT_CPU, 13)
        a.blr()
    return s.routine('current cpu', emit)


def current_kthread(s):
    def emit(a, r, thunk):
        a.label(r.entry)
        a.lwz(3, KPCR_CURRENT_THREAD, 13)
        a.blr()
    return s.routine('current kthread', emit)


def kthread_cpu(s):
    def emit(a, r, thunk):
        a.label(r.entry)
        a.lbz(3, KTHREAD_CURRENT_CPU, 3)
        a.blr()
    return s.routine('kthread cpu', emit)


def kthread_words(s):
    """Copies KTHREAD_WORDS of the KTHREAD at r3 to r4."""
    def emit(a, r, thunk):
        a.label(r.entry)
        for i, offset in enumerate(KTHREAD_WORDS):
            a.lwz(5, offset, 3)
            a.stw(5, 4 * i, 4)
        a.li(3, 0)
        a.blr()
    return s.routine('kthread words', emit)


def create(s, flags, msg):
    """Creates a thread at a probe taking `msg`, waiting for it to record
    unless it starts suspended."""
    handle_out = s.alloc(4, 'thread handle')
    thread = s.variable('thread')
    running = {} if flags & SUSPENDED else {'wait': msg, 'capture': [msg]}
    s.call('ExCreateThread', handle_out, 0, 0, 0, probe(s), msg, flags,
           store=[(thread, handle_out)], name='create', **running)
    return thread


def reference(s, thread):
    object_out = s.alloc(4, 'kthread')
    kthread = s.variable('kthread')
    s.call('ObReferenceObjectByHandle', thread, 0, object_out,
           store=[(kthread, object_out)], name='reference')
    return kthread


def resume(s, thread, msg):
    suspend_count = s.alloc(4, 'suspend count')
    s.call('NtResumeThread', thread, suspend_count, wait=msg,
           capture=[suspend_count, msg], name='resume')


def finish(s, thread):
    timeout = s.alloc(8, 'exit timeout',
                      struct.pack('>q', -EXIT_TIMEOUT_MS * 10000))
    s.call('NtWaitForSingleObjectEx', thread, USER_MODE, 0, timeout,
           name='wait for exit')
    s.call('NtClose', thread, name='close')


def placement(corpus, name, flags):
    s = corpus.scenario(name)
    s.call(current_cpu(s), name='parent cpu')
    finish(s, create(s, flags, message(s)))


def masks(corpus):
    s = corpus.scenario('KeSetAffinityThread masks')
    msg = message(s)
    thread = create(s, SUSPENDED, msg)
    kthread = reference(s, thread)
    cpu = kthread_cpu(s)
    for mask in MASKS:
        previous = s.alloc(4, f'previous before {mask:#x}')
        s.call('KeSetAffinityThread', kthread, mask, previous,
               capture=[previous], name=f'set {mask:#x}')
        s.call(cpu, kthread, name=f'cpu after {mask:#x}')
    s.call('ObDereferenceObject', kthread, void=True)
    resume(s, thread, msg)
    finish(s, thread)


def priorities(corpus):
    s = corpus.scenario('KeSetBasePriorityThread increments')
    msg = message(s)
    thread = create(s, SUSPENDED, msg)
    kthread = reference(s, thread)
    words = kthread_words(s)
    initial = s.alloc(4 * len(KTHREAD_WORDS), 'initial words')
    s.call(words, kthread, initial, capture=[initial], name='initial words')
    s.call('KeQueryBasePriorityThread', kthread, name='initial base')
    for increment in INCREMENTS:
        s.call('KeSetBasePriorityThread', kthread, increment,
               name=f'set {increment:+d}')
        s.call('KeQueryBasePriorityThread', kthread,
               name=f'base after {increment:+d}')
        after = s.alloc(4 * len(KTHREAD_WORDS), f'words after {increment:+d}')
        s.call(words, kthread, after, capture=[after],
               name=f'words after {increment:+d}')
    s.call('ObDereferenceObject', kthread, void=True)
    resume(s, thread, msg)
    finish(s, thread)


def moved_parent(corpus):
    s = corpus.scenario('ExCreateThread, parent moved to CPU 3')
    where = current_cpu(s)
    me = s.variable('executor')
    s.call(current_kthread(s), store=[(me, 'r3')], void=True,
           name='executor kthread')
    previous = s.alloc(4, 'previous affinity')
    restore = s.variable('previous affinity')
    s.call('KeSetAffinityThread', me, 1 << 3, previous, capture=[previous],
           store=[(restore, previous)], name='move to CPU 3')
    s.call(where, name='parent cpu')
    finish(s, create(s, 0, message(s)))
    restored = s.alloc(4, 'affinity before restore')
    s.call('KeSetAffinityThread', me, restore, restored, capture=[restored],
           name='move back')
    s.call(where, name='parent cpu after move back')


def build(corpus):
    s = corpus.scenario('background processors')
    s.call('KeQueryBackgroundProcessors', name='query')
    placement(corpus, 'ExCreateThread, AffinityCpu4', AFFINITY_CPU4)
    placement(corpus, 'ExCreateThread, no affinity', 0)
    placement(corpus, 'ExCreateThread, SystemThread', SYSTEM_THREAD)
    masks(corpus)
    priorities(corpus)
    moved_parent(corpus)
