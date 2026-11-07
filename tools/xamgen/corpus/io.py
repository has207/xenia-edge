"""File corpus: a plain open, opens of a path's target directory, which open
the directory holding the last component and say whether that exists, failed
opens, a file's basic information, times set on a file, a write past the end
of a sized file, an overwritten file sized again, a rename to a \\?? path
and back, and names under \\System?? next to \\??. Everything is under
game:\\krnlgen, which the first scenario makes.

The target directory's handle is checked through its attributes. A failed
open's handle is captured instead of closed. Basic information is captured
from its attributes on, the times being the host's, except the times the
corpus sets.
"""

import datetime
import struct

DOS_DEVICES = 0xFFFFFFFD
OBJ_CASE_INSENSITIVE = 0x40
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
DELETE = 0x00010000
SYNCHRONIZE = 0x00100000
FILE_READ_ATTRIBUTES = 0x80
FILE_WRITE_ATTRIBUTES = 0x100
FILE_ATTRIBUTE_READONLY = 0x1
FILE_ATTRIBUTE_DIRECTORY = 0x10
FILE_ATTRIBUTE_NORMAL = 0x80
FILE_SHARE_READ_WRITE = 3
FILE_OPEN = 1
FILE_CREATE = 2
FILE_OPEN_IF = 3
FILE_OVERWRITE_IF = 5
FILE_DIRECTORY_FILE = 0x1
FILE_NO_INTERMEDIATE_BUFFERING = 0x8
FILE_SYNCHRONOUS_IO_NONALERT = 0x20
FILE_NON_DIRECTORY_FILE = 0x40
OPEN_TARGET_DIRECTORY = 0x4
FILE_BASIC_INFORMATION = 4
BASIC_INFORMATION_SIZE = 0x28
FILE_RENAME_INFORMATION = 10
FILE_POSITION_INFORMATION = 14
FILE_ALLOCATION_INFORMATION = 19
FILE_END_OF_FILE_INFORMATION = 20
FILE_NETWORK_OPEN_INFORMATION = 34
NETWORK_OPEN_INFORMATION_SIZE = 0x38

ROOT = 'game:\\krnlgen'
FILE = ROOT + '\\file'
SYSTEM_LINK = '\\System??\\xgsys:'


def filetime(year, month, day, hour, minute, second, ticks):
    """100 ns units since 1601, `ticks` of them past `second`."""
    delta = (datetime.datetime(year, month, day, hour, minute, second) -
             datetime.datetime(1601, 1, 1))
    return (delta.days * 86400 + delta.seconds) * 10**7 + ticks


# Odd seconds and parts of one, so rounding to a coarser unit shows. The last
# is just past an even second, where rounding up and to the nearest differ.
CREATION_TIME = filetime(2021, 3, 4, 5, 6, 7, 1234567)
ACCESS_TIME = filetime(2022, 4, 5, 6, 7, 9, 2345678)
WRITE_TIME = filetime(2023, 5, 6, 7, 8, 11, 3456789)
REWRITE_TIME = filetime(2024, 6, 7, 8, 9, 13, 4567891)
EVEN_WRITE_TIME = filetime(2025, 7, 8, 9, 10, 12, 1)


def ansi(s, text, name):
    """An ANSI_STRING holding `text`."""
    data = s.string(text, name)
    return s.alloc(8, name, struct.pack('>HHI', len(text), len(text) + 1,
                                        data.address))


def attributes(s, path, root=DOS_DEVICES):
    """OBJECT_ATTRIBUTES naming `path` under `root`, \\?? by default."""
    return s.alloc(0xC, 'attributes', struct.pack(
        '>III', root, ansi(s, path, 'name').address, OBJ_CASE_INSENSITIVE))


def create(s, path, access, file_attributes, disposition, create_options,
           options=None, capture_status=True, capture_handle=False,
           variable='handle', root=DOS_DEVICES):
    """NtCreateFile, or IoCreateFile with `options`. Returns the handle's
    Variable."""
    handle_out = s.alloc(4, 'handle')
    iosb = s.alloc(8, 'iosb')
    args = [handle_out, access, attributes(s, path, root), iosb, 0,
            file_attributes, FILE_SHARE_READ_WRITE, disposition,
            create_options]
    export = 'NtCreateFile'
    if options is not None:
        export = 'IoCreateFile'
        args.append(options)
    handle = s.variable(variable)
    capture = ([iosb] if capture_status else []) + (
        [handle_out] if capture_handle else [])
    s.call(export, *args, capture=capture, store=[(handle, handle_out)])
    return handle


def query_attributes(s, handle, times=False, end_of_file=False):
    """Captures the attributes, or with `times` the four times or with
    `end_of_file` the end of file instead."""
    iosb = s.alloc(8, 'query iosb')
    # Zeroed, so the padding after the attributes reads alike written or not.
    info = s.alloc(NETWORK_OPEN_INFORMATION_SIZE, 'network open information',
                   fill=0)
    if times:
        part = info.prefix(0x20)
    elif end_of_file:
        part = (info + 0x28).prefix(8)
    else:
        part = info + 0x30
    s.call('NtQueryInformationFile', handle, iosb, info,
           NETWORK_OPEN_INFORMATION_SIZE, FILE_NETWORK_OPEN_INFORMATION,
           capture=[iosb, part])


def query_full_attributes(s, path, root=0):
    """NtQueryFullAttributesFile, capturing the attributes."""
    info = s.alloc(NETWORK_OPEN_INFORMATION_SIZE, 'network open information',
                   fill=0)
    s.call('NtQueryFullAttributesFile', attributes(s, path, root), info,
           capture=[info + 0x30], name=f'attributes of {path}')


def query_basic(s, handle, length=BASIC_INFORMATION_SIZE):
    iosb = s.alloc(8, 'query iosb')
    info = s.alloc(BASIC_INFORMATION_SIZE, 'basic information')
    s.call('NtQueryInformationFile', handle, iosb, info, length,
           FILE_BASIC_INFORMATION, capture=[iosb, info + 0x20])


def set_information(s, handle, info_class, data, name, status_only=False):
    iosb = s.alloc(8, 'set iosb')
    info = s.alloc(len(data), 'information', data)
    s.call('NtSetInformationFile', handle, iosb, info, len(data), info_class,
           capture=[iosb.prefix(4) if status_only else iosb], name=name)


def set_basic(s, handle, file_attributes, at=0x20):
    """Sets only the attributes, the times left 0, putting them in the word
    at +0x20 or +0x24 with the other 0."""
    data = bytearray(BASIC_INFORMATION_SIZE)
    struct.pack_into('>I', data, at, file_attributes)
    set_information(s, handle, FILE_BASIC_INFORMATION, bytes(data),
                    f'set attributes {file_attributes:#x} at {at:#x}')


def set_times(s, handle, creation, access, write, name):
    """Sets only the times, a 0 one left alone. Like a rename, FATX can leave
    the Information of the cache's last page read, so only the status is
    captured."""
    data = struct.pack('>QQQQI4x', creation, access, write, 0, 0)
    set_information(s, handle, FILE_BASIC_INFORMATION, data, name,
                    status_only=True)


def set_size(s, handle, size):
    """Sets the end of file and then the allocation size, as XeFu titles size
    a file. Like a rename, FATX can leave the Information of the cache's last
    page read, so only the status is captured."""
    data = struct.pack('>Q', size)
    set_information(s, handle, FILE_END_OF_FILE_INFORMATION, data,
                    'set end of file', status_only=True)
    set_information(s, handle, FILE_ALLOCATION_INFORMATION, data,
                    'set allocation size', status_only=True)


def write(s, handle, size=0x200):
    """Writes `size` bytes at the start."""
    iosb = s.alloc(8, 'write iosb')
    data = s.alloc(size, 'data', fill=0x5A)
    offset = s.alloc(8, 'offset', fill=0)
    s.call('NtWriteFile', handle, 0, 0, 0, iosb, data, size, offset,
           capture=[iosb])


def read(s, handle, size):
    """Reads `size` bytes from the start, capturing the first and last 16."""
    iosb = s.alloc(8, 'read iosb')
    data = s.alloc(size, 'read data')
    offset = s.alloc(8, 'read offset', fill=0)
    s.call('NtReadFile', handle, 0, 0, 0, iosb, data, size, offset,
           capture=[iosb, data.prefix(0x10), data + (size - 0x10)])


def rename(s, handle, root, path, name):
    """FileRenameInformation, not replacing, naming `path` under `root`.
    FATX leaves the Information of the cache's last page read through the
    request, 0x1000 on a miss, so only the status is captured."""
    text = s.string(path, 'rename path')
    data = struct.pack('>B3xIHHI', 0, root, len(path), len(path) + 1,
                       text.address)
    set_information(s, handle, FILE_RENAME_INFORMATION, data, name,
                    status_only=True)


def exists(s, path):
    """The target directory open, whose Information says whether `path`
    exists."""
    handle = create(s, path, GENERIC_READ | SYNCHRONIZE, 0, FILE_OPEN,
                    FILE_SYNCHRONOUS_IO_NONALERT, OPEN_TARGET_DIRECTORY)
    s.call('NtClose', handle)


def target_directory(s, path):
    handle = create(s, path, GENERIC_READ | SYNCHRONIZE, 0, FILE_OPEN,
                    FILE_SYNCHRONOUS_IO_NONALERT, OPEN_TARGET_DIRECTORY)
    query_attributes(s, handle)
    s.call('NtClose', handle)


def build(corpus):
    # Whether these exist from an earlier run shows only in what's not
    # captured.
    s = corpus.scenario('tree')
    handle = create(s, ROOT, GENERIC_READ | SYNCHRONIZE,
                    FILE_ATTRIBUTE_DIRECTORY, FILE_OPEN_IF,
                    FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                    capture_status=False)
    s.call('NtClose', handle)
    handle = create(s, FILE, GENERIC_WRITE | SYNCHRONIZE,
                    FILE_ATTRIBUTE_NORMAL, FILE_OVERWRITE_IF,
                    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                    capture_status=False)
    s.call('NtClose', handle)

    s = corpus.scenario('open file')
    handle = create(s, FILE, GENERIC_READ | SYNCHRONIZE, 0, FILE_OPEN,
                    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, 0)
    s.call('NtClose', handle)

    for name, path in (('existing file', FILE),
                       ('missing file', ROOT + '\\missing'),
                       ('directory', ROOT)):
        target_directory(corpus.scenario(f'target of {name}'), path)

    s = corpus.scenario('target in missing directory')
    create(s, ROOT + '\\missing\\file', GENERIC_READ | SYNCHRONIZE, 0,
           FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, OPEN_TARGET_DIRECTORY,
           capture_handle=True)

    for name, path, disposition, create_options in (
            ('open missing file', ROOT + '\\missing', FILE_OPEN,
             FILE_NON_DIRECTORY_FILE),
            ('create existing file', FILE, FILE_CREATE,
             FILE_NON_DIRECTORY_FILE),
            ('open directory as a file', ROOT, FILE_OPEN,
             FILE_NON_DIRECTORY_FILE)):
        create(corpus.scenario(name), path, GENERIC_READ | SYNCHRONIZE, 0,
               disposition, create_options | FILE_SYNCHRONOUS_IO_NONALERT,
               capture_handle=True)

    for name, path, create_options in (
            ('basic information of file', FILE, FILE_NON_DIRECTORY_FILE),
            ('basic information of directory', ROOT, FILE_DIRECTORY_FILE)):
        s = corpus.scenario(name)
        handle = create(s, path, GENERIC_READ | SYNCHRONIZE, 0, FILE_OPEN,
                        create_options | FILE_SYNCHRONOUS_IO_NONALERT)
        query_basic(s, handle)
        s.call('NtClose', handle)

    s = corpus.scenario('basic information short buffer')
    handle = create(s, FILE, GENERIC_READ | SYNCHRONIZE, 0, FILE_OPEN,
                    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT)
    query_basic(s, handle, BASIC_INFORMATION_SIZE - 4)
    s.call('NtClose', handle)

    # A file of its own, opened for its attributes only so a read-only one
    # left by an earlier run still opens. It ends normal again.
    s = corpus.scenario('set basic information')
    handle = create(s, ROOT + '\\attributes',
                    GENERIC_READ | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
                    FILE_ATTRIBUTE_NORMAL, FILE_OPEN_IF,
                    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                    capture_status=False)
    for at in (0x20, 0x24):
        for file_attributes in (FILE_ATTRIBUTE_NORMAL, FILE_ATTRIBUTE_READONLY,
                                0, FILE_ATTRIBUTE_NORMAL):
            set_basic(s, handle, file_attributes, at)
            query_attributes(s, handle)
    set_basic(s, handle, FILE_ATTRIBUTE_DIRECTORY)
    query_attributes(s, handle)
    set_information(s, handle, FILE_POSITION_INFORMATION, bytes(8),
                    'set position')
    s.call('NtClose', handle)

    # Stamped through the handle that wrote it, as Xbox 1 titles under XeFu
    # stamp their cache copies, then read back after the close.
    s = corpus.scenario('set times')
    handle = create(s, ROOT + '\\times',
                    GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                    FILE_ATTRIBUTE_NORMAL, FILE_OVERWRITE_IF,
                    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                    capture_status=False)
    write(s, handle)
    set_times(s, handle, CREATION_TIME, ACCESS_TIME, WRITE_TIME,
              'set all times')
    query_attributes(s, handle, times=True)
    set_times(s, handle, 0, 0, REWRITE_TIME, 'set write time')
    query_attributes(s, handle, times=True)
    set_times(s, handle, 0, 0, EVEN_WRITE_TIME,
              'set write time past an even second')
    query_attributes(s, handle, times=True)
    s.call('NtClose', handle)
    handle = create(s, ROOT + '\\times', GENERIC_READ | SYNCHRONIZE, 0,
                    FILE_OPEN,
                    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT)
    query_attributes(s, handle, times=True)
    set_times(s, handle, 0, 0, WRITE_TIME,
              'set write time without write access')
    query_attributes(s, handle, times=True)
    s.call('NtClose', handle)

    s = corpus.scenario('set end of file')
    handle = create(s, FILE, GENERIC_WRITE | SYNCHRONIZE, 0, FILE_OPEN,
                    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT)
    set_information(s, handle, FILE_END_OF_FILE_INFORMATION, bytes(8),
                    'set end of file')
    s.call('NtClose', handle)

    # Sized through one handle, then written unbuffered through another in
    # whole sectors, the last one running past the end, as XeFu titles install
    # files.
    s = corpus.scenario('write past end of file')
    path = ROOT + '\\sized'
    access = GENERIC_WRITE | FILE_READ_ATTRIBUTES | SYNCHRONIZE
    handle = create(s, path, access, 0, FILE_OVERWRITE_IF,
                    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                    capture_status=False)
    set_size(s, handle, 0x2FB)
    s.call('NtClose', handle)
    handle = create(s, path, access, 0, FILE_OPEN,
                    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
                    FILE_NO_INTERMEDIATE_BUFFERING)
    query_attributes(s, handle, end_of_file=True)
    write(s, handle, 0x400)
    query_attributes(s, handle, end_of_file=True)
    s.call('NtClose', handle)

    # An overwritten file keeps its old bytes, which come back when it's sized
    # again.
    s = corpus.scenario('overwrite then size again')
    path = ROOT + '\\overwritten'
    handle = create(s, path, access, 0, FILE_OVERWRITE_IF,
                    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                    capture_status=False)
    write(s, handle, 0x400)
    s.call('NtClose', handle)
    handle = create(s, path, access, 0, FILE_OVERWRITE_IF,
                    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT)
    set_size(s, handle, 0x400)
    s.call('NtClose', handle)
    handle = create(s, path, GENERIC_READ | SYNCHRONIZE, 0, FILE_OPEN,
                    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT)
    query_attributes(s, handle, end_of_file=True)
    read(s, handle, 0x400)
    s.call('NtClose', handle)

    # A full \\?? name with no root, as XeFu renames, and back by a name under
    # the DOS devices root.
    s = corpus.scenario('rename')
    source = create(s, ROOT + '\\from', GENERIC_WRITE | DELETE | SYNCHRONIZE,
                    FILE_ATTRIBUTE_NORMAL, FILE_OVERWRITE_IF,
                    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                    capture_status=False, variable='source')
    for root, path, name in (
            (0, '\\??\\' + ROOT + '\\to', 'rename under \\??'),
            (DOS_DEVICES, ROOT + '\\from', 'rename back')):
        rename(s, source, root, path, name)
        exists(s, ROOT + '\\to')
        exists(s, ROOT + '\\from')
    s.call('NtClose', source)

    # Full names in \\?? and \\System??, the DOS devices directories of
    # titles and of the system, and a link made in \\System?? to game:'s
    # target. That link outlives the title, so it's deleted first in case an
    # earlier run stopped short, and again at the end.
    s = corpus.scenario('System?? names')
    link_name = ansi(s, SYSTEM_LINK, 'link name')
    s.call('ObDeleteSymbolicLink', link_name, void=True)
    handle_out = s.alloc(4, 'link handle')
    link = s.variable('link')
    s.call('NtOpenSymbolicLinkObject', handle_out,
           attributes(s, '\\??\\game:', 0), store=[(link, handle_out)])
    target_text = s.alloc(0x100, 'target text', fill=0)
    target = s.alloc(8, 'target', struct.pack('>HHI', 0, target_text.size,
                                              target_text.address))
    s.call('NtQuerySymbolicLinkObject', link, target)
    s.call('NtClose', link)
    s.call('ObCreateSymbolicLink', link_name, target)
    for root, path in (
            (0, '\\??\\' + FILE),
            (0, '\\System??\\' + FILE),
            (0, SYSTEM_LINK + '\\krnlgen\\file'),
            (0, '\\sYSTEM??\\XGSYS:\\krnlgen\\file'),
            (0, '\\??\\xgsys:\\krnlgen\\file'),
            (DOS_DEVICES, 'xgsys:\\krnlgen\\file')):
        query_full_attributes(s, path, root)
    handle = create(s, SYSTEM_LINK + '\\krnlgen\\file',
                    GENERIC_READ | SYNCHRONIZE, 0, FILE_OPEN,
                    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                    root=0)
    s.call('NtClose', handle)
    source = create(s, ROOT + '\\from', GENERIC_WRITE | DELETE | SYNCHRONIZE,
                    FILE_ATTRIBUTE_NORMAL, FILE_OVERWRITE_IF,
                    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                    capture_status=False, variable='source')
    rename(s, source, 0, SYSTEM_LINK + '\\krnlgen\\to',
           'rename under \\System??')
    exists(s, ROOT + '\\to')
    rename(s, source, DOS_DEVICES, ROOT + '\\from', 'rename back')
    s.call('NtClose', source)
    s.call('ObDeleteSymbolicLink', link_name)
    query_full_attributes(s, SYSTEM_LINK + '\\krnlgen\\file')

    s = corpus.scenario('full attributes of missing names')
    for path in (ROOT + '\\missing', ROOT + '\\missing\\file',
                 'xgnone:\\file', 'xgnone:'):
        query_full_attributes(s, path, DOS_DEVICES)

    # The size is checked before anything is read, so nothing is mounted.
    s = corpus.scenario('StfsCreateDevice wrong size')
    params = s.alloc(0x58, 'parameters', fill=0)
    s.call('StfsCreateDevice', params, 0x50)
