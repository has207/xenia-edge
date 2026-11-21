"""Rtl corpus: RtlUnicodeStringToAnsiString on a string holding a character
past 0x7F that fits in a byte and one past 0xFF that doesn't, into a buffer
that holds it, one that cuts it short, one with no room and one it allocates.

Buffers start out as 0xCD, so bytes the call leaves alone stay 0xCD. An
allocated buffer's address is the pool's, so only the lengths are captured.
"""

import struct

TEXT = 'AéあZ'
BUFFER_SIZE = 0x10


def unicode_string(s, text):
    data = text.encode('utf-16-be')
    buffer = s.alloc(len(data), 'source buffer', data)
    return s.alloc(8, 'source', struct.pack('>HHI', len(data), len(data),
                                            buffer.address))


def build(corpus):
    for name, capacity in (('fits', BUFFER_SIZE), ('cut short', 3),
                           ('no room', 0)):
        s = corpus.scenario(f'to ANSI, {name}')
        source = unicode_string(s, TEXT)
        buffer = s.alloc(BUFFER_SIZE, 'buffer')
        string = s.alloc(8, 'string', struct.pack('>HHI', 0xCDCD, capacity,
                                                  buffer.address))
        s.call('RtlUnicodeStringToAnsiString', string, source, 0,
               capture=[string.prefix(4), buffer])

    s = corpus.scenario('to ANSI, allocated')
    source = unicode_string(s, TEXT)
    string = s.alloc(8, 'string', fill=0)
    s.call('RtlUnicodeStringToAnsiString', string, source, 1,
           capture=[string.prefix(4)])
    s.call('RtlFreeAnsiString', string, void=True)
