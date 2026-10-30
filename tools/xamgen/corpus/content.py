"""Content corpus: creating, opening, closing and deleting saves, creators,
thumbnails, argument checks, user indices, shared content and the default
device.

Every scenario uses its own file names under the executor's title, deletes
them first in case an earlier run left them and closes and deletes what it
made at the end. Output pointers start filled with 0xCD so a capture shows
whether xam cleared or wrote them.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import identity  # noqa: E402

HDD = 1
SAVED_GAME = 1

CREATE_NEW = 1
CREATE_ALWAYS = 2
OPEN_EXISTING = 3
OPEN_ALWAYS = 4
TRUNCATE_EXISTING = 5
NO_PROFILE_TRANSFER = 0x10

ROOT = 'xgtst'
ROOT2 = 'xgtst2'

# A 1x1 PNG.
PNG = bytes.fromhex(
    '89504e470d0a1a0a0000000d4948445200000001000000010806000000'
    '1f15c4890000000d49444154789c6360f8cff0000000040001f31d0d40'
    '0000000049454e44ae426082')


def content_data(file_name, display='xamgen', device=HDD,
                 content_type=SAVED_GAME):
    data = bytearray(0x134)
    struct.pack_into('>II', data, 0, device, content_type)
    name = display.encode('utf-16-be')[:0xFE]
    data[8:8 + len(name)] = name
    file_name = file_name if isinstance(file_name, bytes) else (
        file_name.encode())
    data[0x108:0x108 + len(file_name)] = file_name
    return bytes(data)


class Content:
    """Calls on one scenario's content data and roots."""

    def __init__(self, s):
        self.s = s
        self.roots = {}

    def data(self, file_name, **kwargs):
        return self.s.alloc(0x134, f'cd {file_name!r}',
                            content_data(file_name, **kwargs))

    def root(self, name):
        if name not in self.roots:
            self.roots[name] = self.s.string(name, f'root {name}')
        return self.roots[name]

    def overlapped(self):
        return self.s.alloc(0x1C, 'overlapped', fill=0)

    def create(self, user, root, data, flags, cache=0, size=0,
               overlapped=False, name=None, faults=False):
        s = self.s
        disposition = s.alloc(4, 'disposition')
        license = s.alloc(4, 'license')
        root = self.root(root) if isinstance(root, str) else root
        capture = [disposition, license]
        args = [user, root, data, flags, disposition, license, cache, size]
        ov = None
        if overlapped:
            ov = self.overlapped()
            args.append(ov)
            capture.append(ov)
        s.call('XamContentCreateEx', *args, name=name, wait=ov,
               capture=capture, faults=faults)

    def close(self, root, overlapped=False, name=None):
        s = self.s
        ov = self.overlapped() if overlapped else None
        s.call('XamContentClose', self.root(root), ov or 0, wait=ov,
               capture=[ov] if ov else [], name=name or f'close {root}')

    def delete(self, user, data, overlapped=False, name=None):
        s = self.s
        ov = self.overlapped() if overlapped else None
        s.call('XamContentDelete', user, data, ov or 0, wait=ov,
               capture=[ov] if ov else [], name=name or 'delete')

    def creator(self, user, data, overlapped=False, name=None):
        s = self.s
        flag = s.alloc(4, 'is creator')
        xuid = s.alloc(8, 'creator xuid')
        ov = self.overlapped() if overlapped else None
        s.call('XamContentGetCreator', user, data, flag, xuid, ov or 0,
               wait=ov, capture=[flag, xuid] + ([ov] if ov else []),
               name=name or 'creator')

    def thumbnail(self, user, data, buffer_size=None, overlapped=False,
                  name=None):
        """Asks for the size alone when `buffer_size` is None, its sentinel
        showing whether it was written."""
        s = self.s
        size = s.alloc(4, 'size', None if buffer_size is None else
                       struct.pack('>I', buffer_size))
        capture = [size]
        buffer = 0
        if buffer_size is not None:
            buffer = s.alloc(max(buffer_size, 8) + 8, 'thumbnail')
            capture.append(buffer)
        ov = self.overlapped() if overlapped else None
        if ov:
            capture.append(ov)
        s.call('XamContentGetThumbnail', user, data, buffer, size, ov or 0,
               wait=ov, capture=capture, name=name or 'get thumbnail')

    def set_thumbnail(self, user, data, image, overlapped=False, name=None):
        s = self.s
        buffer = s.alloc(len(image), 'image', image)
        ov = self.overlapped() if overlapped else None
        s.call('XamContentSetThumbnail', user, data, buffer, len(image),
               ov or 0, wait=ov, capture=[ov] if ov else [],
               name=name or 'set thumbnail')

    def enumerate(self, user, count=4, name='enumerate'):
        """Lists the saves `user` sees, `count` at most, then closes the
        enumerator."""
        s = self.s
        size = s.alloc(4, 'buffer size')
        handle = s.alloc(4, 'handle')
        enumerator = s.variable('enumerator')
        s.call('XamContentCreateEnumerator', user, HDD, SAVED_GAME, 0, count,
               size, handle, capture=[size], store=[(enumerator, handle)],
               name=f'{name} create enumerator')
        items = s.alloc(count * 0x134, 'items')
        returned = s.alloc(4, 'returned')
        s.call('XamEnumerate', enumerator, 0, items, count * 0x134, returned,
               0, capture=[items, returned], name=name)
        s.call('NtClose', enumerator, name=f'{name} close')


def scenario(corpus, name):
    return Content(corpus.scenario(name))


def build(corpus):
    identity(corpus)

    # Content that doesn't exist.
    c = scenario(corpus, 'missing')
    missing = c.data('xg_missing')
    c.delete(0, missing, name='delete first')
    c.creator(0, missing)
    c.creator(0, missing, overlapped=True, name='creator overlapped')
    c.create(0, ROOT, missing, OPEN_EXISTING, name='open existing')
    c.create(0, ROOT, missing, OPEN_EXISTING | NO_PROFILE_TRANSFER,
             name='open existing no profile transfer')
    c.create(0, ROOT, missing, OPEN_EXISTING, overlapped=True,
             name='open existing overlapped')
    c.create(0, ROOT, missing, TRUNCATE_EXISTING, name='truncate existing')
    c.close(ROOT, name='close nothing open')
    c.close(ROOT, overlapped=True, name='close nothing open overlapped')
    c.delete(0, missing, name='delete missing')
    c.delete(0, missing, overlapped=True, name='delete missing overlapped')
    c.thumbnail(0, missing, name='thumbnail size')
    c.thumbnail(0, missing, overlapped=True, name='thumbnail size overlapped')
    c.set_thumbnail(0, missing, PNG, name='set thumbnail')
    c.set_thumbnail(0, missing, PNG, overlapped=True,
                    name='set thumbnail overlapped')
    c.thumbnail(0, missing, name='thumbnail size after set')
    c.delete(0, missing, name='delete after set thumbnail')

    # One save from creation to deletion.
    c = scenario(corpus, 'lifecycle')
    save = c.data('xg_life')
    c.delete(0, save, name='delete first')
    c.create(0, ROOT, save, CREATE_NEW, name='create new')
    c.creator(0, save, name='creator while open')
    c.create(0, ROOT, save, OPEN_EXISTING, name='open again on its root')
    c.create(0, ROOT2, save, CREATE_NEW, name='create new while it exists')
    c.close(ROOT)
    c.close(ROOT, name='close again')
    c.create(0, ROOT, save, OPEN_EXISTING, name='open existing')
    c.close(ROOT)
    c.create(0, ROOT, save, OPEN_ALWAYS, name='open always')
    c.close(ROOT)
    c.create(0, ROOT, save, CREATE_ALWAYS, name='create always')
    c.close(ROOT)
    c.set_thumbnail(0, save, PNG)
    c.thumbnail(0, save, name='thumbnail size')
    c.thumbnail(0, save, len(PNG), name='thumbnail')
    c.thumbnail(0, save, 8, name='thumbnail short buffer')
    c.thumbnail(0, save, 0x100, name='thumbnail large buffer')
    c.thumbnail(0, save, overlapped=True, name='thumbnail size overlapped')
    c.thumbnail(0, save, 8, overlapped=True,
                name='thumbnail short buffer overlapped')
    c.set_thumbnail(0, save, PNG, overlapped=True,
                    name='set thumbnail overlapped')
    c.create(0, ROOT, save, TRUNCATE_EXISTING, name='truncate existing')
    c.close(ROOT)
    c.thumbnail(0, save, name='thumbnail size after truncate')
    # Whether a truncate or an open takes the display name it's given.
    c.create(0, ROOT, c.data('xg_life', display='truncated'),
             TRUNCATE_EXISTING, name='truncate with another display name')
    c.close(ROOT)
    c.enumerate(0, name='enumerate after truncate')
    c.create(0, ROOT, c.data('xg_life', display='opened'), OPEN_EXISTING,
             name='open with another display name')
    c.close(ROOT)
    c.enumerate(0, name='enumerate after open')
    c.creator(0, save, name='creator')
    c.creator(0, save, overlapped=True, name='creator overlapped')
    c.create(0, ROOT, save, OPEN_EXISTING, overlapped=True,
             name='open existing overlapped')
    c.close(ROOT, overlapped=True, name='close overlapped')
    c.delete(0, save)
    c.delete(0, save, name='delete again')
    c.creator(0, save, name='creator after delete')

    # A root that's already mounted.
    c = scenario(corpus, 'root in use')
    a = c.data('xg_col_a')
    b = c.data('xg_col_b')
    c.delete(0, a, name='delete a first')
    c.delete(0, b, name='delete b first')
    c.create(0, ROOT, a, CREATE_NEW, name='create a')
    c.create(0, ROOT, b, CREATE_NEW, name='create new b on its root')
    c.create(0, ROOT, b, OPEN_ALWAYS, name='open always b on its root')
    c.create(0, ROOT2, b, CREATE_NEW, name='create b elsewhere')
    c.close(ROOT2)
    c.create(0, ROOT, b, OPEN_EXISTING, name='open existing b on its root')
    c.create(0, ROOT, b, TRUNCATE_EXISTING, name='truncate b on its root')
    c.create(0, ROOT2, b, OPEN_EXISTING, name='b still there')
    c.close(ROOT2)
    c.create(0, ROOT, b, CREATE_ALWAYS, name='create always b on its root')
    c.create(0, ROOT2, b, OPEN_EXISTING, name='b after create always')
    c.close(ROOT2)
    c.create(0, ROOT2, a, CREATE_ALWAYS, name='create always a while open')
    c.close(ROOT2)
    c.close(ROOT)
    c.delete(0, a, name='delete a')
    c.delete(0, b, name='delete b')

    # Argument checks, each with the outputs filled to see what's cleared.
    c = scenario(corpus, 'arguments')
    good = c.data('xg_args')
    c.delete(0, good, name='delete first')
    c.create(0, ROOT, good, CREATE_NEW, cache=0xEF001, name='cache too big')
    c.create(0, ROOT, good, CREATE_NEW, cache=0xEF000, name='largest cache')
    c.close(ROOT)
    c.delete(0, good, name='delete after largest cache')
    for root in ('xg:t', 'xg.t', 'x-g', '\\??\\xgtst', '\\SYSTEM??\\xgtst',
                 '\\??\\', ''):
        c.create(0, root, good, OPEN_EXISTING, name=f'root {root!r}')
    c.create(0, 0, good, OPEN_EXISTING, name='root NULL')
    c.create(0, 'xg_t', good, CREATE_NEW, name='root with underscore')
    c.close('xg_t')
    c.delete(0, good, name='delete after underscore root')
    for file_name in (b'', b'xg\xe9x', b'\xe9xgx'):
        data = c.data(file_name)
        c.create(0, ROOT, data, CREATE_NEW, name=f'file name {file_name!r}')
        c.close(ROOT, name=f'close after {file_name!r}')
        c.delete(0, data, name=f'delete {file_name!r}')
    for flags in (0, 6, 7, 0xF):
        c.create(0, ROOT, good, flags, name=f'flags {flags:#x}')
        c.close(ROOT, name=f'close after flags {flags:#x}')
    c.delete(0, good, name='delete last')

    # User indices on content one user made.
    c = scenario(corpus, 'users')
    save = c.data('xg_users')
    c.delete(0, save, name='delete first')
    c.create(0, ROOT, save, CREATE_NEW, name='create as user 0')
    c.close(ROOT)
    for user in (1, 4, 0xFD, 0xFE, 0xFF):
        c.creator(user, save, name=f'creator user {user:#x}')
        c.create(user, ROOT, save, OPEN_EXISTING, name=f'open user {user:#x}')
        c.close(ROOT, name=f'close after user {user:#x}')
    c.creator(1, save, overlapped=True, name='creator user 1 overlapped')
    c.create(1, ROOT, save, OPEN_EXISTING, overlapped=True,
             name='open user 1 overlapped')
    c.delete(1, save, name='delete user 1')
    c.delete(1, save, overlapped=True, name='delete user 1 overlapped')
    c.thumbnail(1, save, name='thumbnail user 1')
    c.delete(0, save, name='delete as user 0')
    shared = c.data('xg_shared')
    c.delete(0xFE, shared, name='delete shared first')
    c.create(0xFE, ROOT, shared, CREATE_NEW, name='create user 0xfe')
    c.close(ROOT)
    c.creator(0xFE, shared, name='creator user 0xfe')
    c.creator(0, shared, name='creator user 0 of shared')
    c.create(0, ROOT, shared, OPEN_EXISTING, name='open shared as user 0')
    c.close(ROOT)
    c.delete(0xFE, shared, name='delete shared')

    # A first file name byte with bit 0x80 set, which xam's path builder takes
    # as content under XUID 0 named without that bit.
    c = scenario(corpus, 'shared flag')
    flagged = c.data(b'\xe9xgsh')
    plain = c.data('ixgsh')
    c.delete(0, flagged, name='delete flagged first')
    c.delete(0xFE, plain, name='delete plain first')
    c.create(0, ROOT, flagged, CREATE_NEW, name='create flagged as user 0')
    c.close(ROOT)
    c.creator(0, flagged, name='creator flagged user 0')
    c.creator(0xFE, flagged, name='creator flagged user 0xfe')
    c.creator(0xFE, plain, name='creator plain user 0xfe')
    c.creator(0, plain, name='creator plain user 0')
    c.create(0xFE, ROOT, plain, OPEN_EXISTING, name='open plain as user 0xfe')
    c.close(ROOT)
    c.enumerate(0, name='enumerate user 0 with only shared')
    c.create(0, ROOT, plain, CREATE_NEW, name='create plain as user 0')
    c.close(ROOT)
    c.enumerate(0, name='enumerate user 0')
    c.enumerate(0xFE, name='enumerate user 0xfe')
    c.delete(0, plain, name='delete plain as user 0')
    c.delete(0xFE, plain, name='delete plain as user 0xfe')
    c.delete(0, flagged, name='delete flagged after')

    # xam reports the first mounted device, the hard drive here.
    s = corpus.scenario('default device')
    device = s.alloc(4, 'device')
    s.call('XamContentGetDefaultDevice', device, capture=[device])
