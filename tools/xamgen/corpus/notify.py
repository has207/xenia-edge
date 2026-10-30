"""Notify corpus: how a notification listener queues what XNotifyBroadcast
sends, in which order XNotifyGetNext hands it out and when the listener's
handle is signaled.

The notifications are custom ones (area 3), which titles get from their own
player list UI, and a friends presence change (area 2). Every listener of those
areas sees them, as on any console.
"""

import struct

FRIENDS = 1 << 2
CUSTOM = 1 << 3
MAX_VERSION = 1

FRIEND_PRESENCE = 0x04000001
ACTION_PRESSED = 0x06000003
GAMERCARD = 0x06010004

USER_MODE = 1
WAIT_SIGNALED = 0
WAIT_TIMEOUT = 0x102


def zero_timeout(s):
    return s.alloc(8, 'no wait', struct.pack('>q', 0))


def listener(s, mask):
    # The handle is kept but not compared, its value differs between runs.
    handle = s.variable('listener')
    s.call('XamNotifyCreateListener', mask, MAX_VERSION,
           store=[(handle, 'r3')], void=True, name='create listener')
    return handle


def poll(s, handle, name):
    s.call('NtWaitForSingleObjectEx', handle, USER_MODE, 0, zero_timeout(s),
           name=name)


def next_notification(s, handle, match, name):
    id_ptr = s.alloc(4, 'id')
    param = s.alloc(4, 'param')
    s.call('XNotifyGetNext', handle, match, id_ptr, param,
           capture=[id_ptr, param], name=name)


def build(corpus):
    s = corpus.scenario('signaled once')
    handle = listener(s, CUSTOM)
    s.call('XNotifyBroadcast', ACTION_PRESSED, 1)
    poll(s, handle, 'poll')
    poll(s, handle, 'poll again')
    next_notification(s, handle, 0, 'next')
    next_notification(s, handle, 0, 'next when empty')
    poll(s, handle, 'poll when empty')
    s.call('NtClose', handle)

    s = corpus.scenario('one entry per id')
    handle = listener(s, CUSTOM)
    s.call('XNotifyBroadcast', GAMERCARD, 1, name='gamercard')
    s.call('XNotifyBroadcast', ACTION_PRESSED, 2, name='action pressed')
    s.call('XNotifyBroadcast', ACTION_PRESSED, 3, name='action pressed again')
    next_notification(s, handle, 0, 'next')
    next_notification(s, handle, 0, 'next again')
    next_notification(s, handle, 0, 'next when empty')
    s.call('NtClose', handle)

    s = corpus.scenario('repeat signals again')
    handle = listener(s, CUSTOM)
    s.call('XNotifyBroadcast', ACTION_PRESSED, 1, name='action pressed')
    poll(s, handle, 'poll')
    s.call('XNotifyBroadcast', ACTION_PRESSED, 2, name='action pressed again')
    poll(s, handle, 'poll after the repeat')
    next_notification(s, handle, 0, 'next')
    poll(s, handle, 'poll when empty')
    s.call('NtClose', handle)

    s = corpus.scenario('area order')
    handle = listener(s, FRIENDS | CUSTOM)
    s.call('XNotifyBroadcast', ACTION_PRESSED, 1, name='action pressed')
    s.call('XNotifyBroadcast', FRIEND_PRESENCE, 2, name='friend presence')
    next_notification(s, handle, 0, 'next')
    next_notification(s, handle, 0, 'next again')
    s.call('NtClose', handle)

    s = corpus.scenario('two areas')
    handle = listener(s, FRIENDS | CUSTOM)
    s.call('XNotifyBroadcast', ACTION_PRESSED, 1, name='action pressed')
    s.call('XNotifyBroadcast', FRIEND_PRESENCE, 2, name='friend presence')
    next_notification(s, handle, ACTION_PRESSED, 'next action pressed')
    poll(s, handle, 'poll with a friend presence left')
    next_notification(s, handle, ACTION_PRESSED, 'next action pressed again')
    next_notification(s, handle, 0, 'next')
    poll(s, handle, 'poll when empty')
    s.call('NtClose', handle)
