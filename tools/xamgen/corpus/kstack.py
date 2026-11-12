"""Kernel stack corpus: where MmCreateKernelStack puts a stack for each size and
type.

The top it returns changes from run to run, so a routine reduces it to its
high byte, the region: 4 KB pages for a size that isn't a multiple of 64 KB,
64 KB pages otherwise, in the title's ranges for type 1 and the system's for
type 2. Type 0 takes the caller's process type, a title here. Each scenario
then fills the whole stack and deletes it. Any other type returns 0.

The thread scenarios create a suspended thread and read the region and size
of its stack from the KTHREAD, then let it run to its exit.
"""

import struct

PATTERN = 0x5AA5C33C
SUSPENDED = 1
USER_MODE = 1
EXIT_TIMEOUT_MS = 1000
KTHREAD_STACK_BASE = 0x5C
KTHREAD_STACK_LIMIT = 0x60

STACKS = (
    ('32 KB, caller type', 0x8000, 0),
    ('16 KB, title', 0x4000, 1),
    ('38 KB unrounded, title', 0x9800, 1),
    ('64 KB, title', 0x10000, 1),
    ('256 KB, caller type', 0x40000, 0),
    ('32 KB, system', 0x8000, 2),
    ('64 KB, system', 0x10000, 2),
)


THREADS = (
    ('32 KB', 0x8000),
    ('8 KB, raised to the minimum', 0x2000),
    ('38 KB unrounded', 0x9800),
    ('64 KB', 0x10000),
    ('default size', 0),
)


def region_of(s):
    def emit(a, r, thunk):
        a.label(r.entry)
        a.rlwinm(3, 3, 8, 24, 31)
        a.blr()
    return s.routine('region', emit)


def thread_exit(s):
    def emit(a, r, thunk):
        a.label(r.entry)
        a.li(3, 0)
        a.blr()
    return s.routine('thread start', emit)


def stack_of(s):
    """Returns the region of the KTHREAD at r3's stack and writes its size
    at r4."""
    def emit(a, r, thunk):
        a.label(r.entry)
        a.lwz(5, KTHREAD_STACK_BASE, 3)
        a.lwz(6, KTHREAD_STACK_LIMIT, 3)
        a.subf(6, 6, 5)
        a.stw(6, 0, 4)
        a.rlwinm(3, 5, 8, 24, 31)
        a.blr()
    return s.routine('thread stack', emit)


def thread_scenario(corpus, name, size):
    s = corpus.scenario(f'thread stack {name}')
    start = thread_exit(s)
    stack = stack_of(s)
    handle_out = s.alloc(4, 'thread handle')
    thread = s.variable('thread')
    s.call('ExCreateThread', handle_out, size, 0, 0, start, 0, SUSPENDED,
           store=[(thread, handle_out)], name='create')
    object_out = s.alloc(4, 'kthread')
    kthread = s.variable('kthread')
    s.call('ObReferenceObjectByHandle', thread, 0, object_out,
           store=[(kthread, object_out)], name='reference')
    stack_size = s.alloc(4, 'stack size')
    s.call(stack, kthread, stack_size, capture=[stack_size], name='stack')
    s.call('ObDereferenceObject', kthread, void=True)
    suspend_count = s.alloc(4, 'suspend count')
    s.call('NtResumeThread', thread, suspend_count, capture=[suspend_count],
           name='resume')
    timeout = s.alloc(8, 'exit timeout',
                      struct.pack('>q', -EXIT_TIMEOUT_MS * 10000))
    s.call('NtWaitForSingleObjectEx', thread, USER_MODE, 0, timeout,
           name='wait for exit')
    s.call('NtClose', thread, name='close')


def build(corpus):
    for name, size, kind in STACKS:
        s = corpus.scenario(f'kernel stack {name}')
        region = region_of(s)
        top = s.variable('top')
        rounded = (size + 0xFFF) & ~0xFFF
        s.call('MmCreateKernelStack', size, kind, name='create', void=True,
               store=[(top, 'r3')])
        s.call(region, top, name='region')
        s.call('RtlFillMemoryUlong', top + -rounded, rounded, PATTERN,
               name='fill', void=True)
        s.call('MmDeleteKernelStack', top, top + -rounded, name='delete',
               void=True)

    s = corpus.scenario('kernel stack of type 3')
    s.call('MmCreateKernelStack', 0x8000, 3, name='create')

    for name, size in THREADS:
        thread_scenario(corpus, name, size)
