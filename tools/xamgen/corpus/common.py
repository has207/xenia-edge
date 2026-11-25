"""Pieces corpora share: the identity scenario every corpus that compares
profile values starts with, and a probe for where a thread runs."""

import struct

PENDING = 0x3E5
KPCR_CURRENT_THREAD = 0x100
KPCR_CURRENT_CPU = 0x10C
# Priority and process type, base priority to boost disable, suspend count to
# current CPU, priority class to max dynamic priority.
KTHREAD_WORDS = (0x70, 0xB8, 0xBC, 0xC8)
MESSAGE_SIZE = 0xC + 4 * len(KTHREAD_WORDS)


def identity(corpus):
    """The XUID and gamertag run.py replaces in comparisons."""
    s = corpus.scenario('identity')
    xuid = s.alloc(8, 'xuid')
    s.call('XamUserGetXUID', 0, 1, xuid, capture=[xuid])
    gamertag = s.alloc(16, 'gamertag')
    s.call('XamUserGetName', 0, gamertag, 16, capture=[gamertag])


def probe(s):
    """A thread start or callback taking a message() at r3. It records the
    message pointer, its KPCR's processor and KTHREAD_WORDS of its KTHREAD,
    then clears the message's first word, which a step can wait on."""
    def emit(a, r, thunk):
        a.label(r.entry)
        a.stw(3, 4, 3)
        a.lbz(4, KPCR_CURRENT_CPU, 13)
        a.stw(4, 8, 3)
        a.lwz(5, KPCR_CURRENT_THREAD, 13)
        for i, offset in enumerate(KTHREAD_WORDS):
            a.lwz(4, offset, 5)
            a.stw(4, 0xC + 4 * i, 3)
        a.sync()
        a.li(4, 0)
        a.stw(4, 0, 3)
        a.li(3, 0)
        a.blr()
    return s.routine('probe', emit)


def message(s):
    """A probe's message, pending until the probe runs."""
    return s.alloc(MESSAGE_SIZE, 'message', struct.pack('>I', PENDING), fill=0)


def without_cpu(msg):
    """Captures of a probe's message that leave out the KPCR processor and
    the KTHREAD word ending in the current CPU."""
    return [msg.prefix(0x8), (msg + 0xC).prefix(0x8), (msg + 0x18).prefix(0x4)]
