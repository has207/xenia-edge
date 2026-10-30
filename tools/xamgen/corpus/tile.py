"""Tile corpus: what XamReadTileToTexture returns and writes when the tile
can't be read, for the signed-in user, for XUSER_INDEX_NONE (how gamer pictures
are read by key) and for an empty slot, with and without an overlapped.

Slot 0 is the signed-in profile and slot 1 is empty. Each call reads into a
64x64 texture that starts out as 0xCD, and its first bytes are captured to see
whether a failed read writes anything.

Every failure returns ERROR_FUNCTION_FAILED. The extended error depends on
where xam's lookup gave up: an empty slot is ERROR_NO_SUCH_USER and a bad tile
type is file not found, but an unknown title's icon is 0x80151802 and an
unknown key E_INVALIDARG, which Xenia doesn't tell apart from a missing tile.
Those two only compare the result and the byte count.
"""

import struct

GAME_ICON = 1
GAMER_TILE = 2
BAD_TILE_TYPE = 0x20
USER_NONE = 0xFE
EMPTY_SLOT = 1
NO_TITLE = 0x12345678
DASHBOARD_TITLE = 0xFFFE07D1
NO_TILE = 0x0001FFFF0001FFFF
STRIDE = 64 * 4
HEIGHT = 64

# InternalLow, InternalHigh and dwExtendedError hold sentinels to show what's
# written, the event and completion routine are null.
OVERLAPPED = struct.pack('>7I', 0xCDCDCDCD, 0xCDCDCDCD, 0, 0, 0, 0,
                         0xCDCDCDCD)


def read(corpus, name, tile_type, title_id, tile_id, user_index,
         extended_error=True, texture=True):
    s = corpus.scenario(name)
    for overlapped in (False, True):
        captures = []
        buffer = 0
        if texture:
            buffer = s.alloc(STRIDE * HEIGHT, 'texture')
            captures.append(buffer.prefix(0x40))
        ov = 0
        if overlapped:
            ov = s.alloc(len(OVERLAPPED), 'overlapped', OVERLAPPED)
            # InternalLow and InternalHigh, then the extended error at 0x18.
            captures.append(ov if extended_error else ov.prefix(8))
        s.call('XamReadTileToTexture', tile_type, title_id, tile_id,
               user_index, buffer, STRIDE, HEIGHT, ov,
               wait=ov if overlapped else None, capture=captures,
               name='overlapped' if overlapped else 'synchronous')


def build(corpus):
    read(corpus, 'missing game icon', GAME_ICON, NO_TITLE, 0, 0,
         extended_error=False)
    read(corpus, 'missing gamer tile by key', GAMER_TILE, DASHBOARD_TITLE,
         NO_TILE, USER_NONE, extended_error=False)
    read(corpus, 'empty slot', GAMER_TILE, DASHBOARD_TITLE, NO_TILE,
         EMPTY_SLOT)
    read(corpus, 'bad tile type', BAD_TILE_TYPE, NO_TITLE, 0, 0)
    read(corpus, 'null texture', GAME_ICON, NO_TITLE, 0, 0,
         extended_error=False, texture=False)
