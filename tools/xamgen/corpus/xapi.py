"""Xapi corpus: the timeout XapiFormatTimeOut writes and the pointer it
returns. The WaitForSingleObject(Ex) scenarios record the result and the last
error for a signaled, an unsignaled and an invalid handle, a timeout before a
timer fires, a user APC and an alertable BOOL with a clear low byte.

Every finite wait is on a timer due in 100 ms so a wait that gets a wrong
timeout still ends when it fires. Each wait is preceded by
RtlSetLastNTError(STATUS_ACCESS_DENIED) so a last error the wait leaves alone
reads back as 5. SetLastError would do but Xenia doesn't implement it.
"""

import struct

NOTIFICATION = 0
USER_MODE = 1
CURRENT_THREAD = 0xFFFFFFFE
INFINITE = 0xFFFFFFFF
INVALID_HANDLE = 0x00BADBAD
ACCESS_DENIED = 0xC0000022
DUE_MS = 100


def wait(s, export, handle, milliseconds, *alertable, name, capture=()):
    s.call('RtlSetLastNTError', ACCESS_DENIED, name='set last error')
    s.call(export, handle, milliseconds, *alertable, name=name,
           capture=capture)
    s.call('GetLastError', name=f'last error after {name}')


def due_timer(s):
    """A notification timer due in DUE_MS."""
    handle = s.alloc(4, 'handle')
    timer = s.variable('timer')
    s.call('NtCreateTimer', handle, 0, NOTIFICATION,
           store=[(timer, handle)])
    due = s.alloc(8, 'due', struct.pack('>q', -DUE_MS * 10000))
    s.call('NtSetTimerEx', timer, due, 0, USER_MODE, 0, 0, 0, 0)
    return timer


def build(corpus):
    s = corpus.scenario('XapiFormatTimeOut')
    for milliseconds in (0, 1, 1000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFE,
                         INFINITE):
        timeout = s.alloc(8, f'{milliseconds:#x} ms')
        s.call('XapiFormatTimeOut', timeout, milliseconds,
               name=f'{milliseconds:#x} ms', capture=[timeout])
    s.call('XapiFormatTimeOut', 0, INFINITE, name='null, INFINITE')

    s = corpus.scenario('WaitForSingleObjectEx event')
    handle = s.alloc(4, 'handle')
    event = s.variable('event')
    s.call('NtCreateEvent', handle, 0, NOTIFICATION, 0,
           store=[(event, handle)])
    wait(s, 'WaitForSingleObjectEx', event, 0, 0, name='poll unsignaled')
    s.call('NtSetEvent', event, 0)
    wait(s, 'WaitForSingleObjectEx', event, 0, 0, name='poll signaled')
    wait(s, 'WaitForSingleObject', event, INFINITE,
         name='WaitForSingleObject signaled')
    s.call('NtClose', event)
    wait(s, 'WaitForSingleObjectEx', INVALID_HANDLE, 0, 0,
         name='invalid handle')

    s = corpus.scenario('WaitForSingleObjectEx timer')
    timer = due_timer(s)
    wait(s, 'WaitForSingleObjectEx', timer, 20, 0,
         name='wait 20 ms before it fires')
    wait(s, 'WaitForSingleObjectEx', timer, 1000, 0,
         name='wait for it to fire')
    s.call('NtClose', timer)

    # The APC is queued before the wait so an alertable wait delivers it at
    # once.
    s = corpus.scenario('WaitForSingleObjectEx user APC')
    timer = due_timer(s)
    normal = s.recorder('normal routine', args=3)
    s.call('NtQueueApcThread', CURRENT_THREAD, normal, 1, 2, 3)
    wait(s, 'WaitForSingleObjectEx', timer, 1000, 1, name='alertable wait',
         capture=[normal.log])
    s.call('NtWaitForSingleObjectEx', timer, USER_MODE, 0, 0,
           name='wait for the timer')
    s.call('NtClose', timer)

    # 0x100 has no alertable bit in its low byte so the APC stays queued
    # until the alertable sleep.
    s = corpus.scenario('WaitForSingleObjectEx alertable 0x100')
    timer = due_timer(s)
    normal = s.recorder('normal routine', args=3)
    s.call('NtQueueApcThread', CURRENT_THREAD, normal, 1, 2, 3)
    wait(s, 'WaitForSingleObjectEx', timer, 1000, 0x100,
         name='wait with alertable 0x100', capture=[normal.log])
    interval = s.alloc(8, 'interval', struct.pack('>q', -10 * 10000))
    s.call('KeDelayExecutionThread', USER_MODE, 1, interval,
           name='alertable sleep', capture=[normal.log])
    s.call('NtClose', timer)
