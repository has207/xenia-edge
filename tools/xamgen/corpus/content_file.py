"""Content file corpus: XamContentOpenFile mounting a package by its file
name, called as xam's own callers do (user 0xFF, flags 0x43 and the fifth
argument 0), with the license mask pointer filled to see what's written.

The arguments scenario opens a missing file, a file that isn't a package and
bad root names. The package scenario makes a shared save holding one file,
closes it and reopens it by its path under the content partition: on its root,
on a root already in use, on two roots at once and as other users. That save
is why the corpus has a title of its own. Xenia runs `--only arguments`, since
it keeps a package as a folder and its content partition lists folders once at
startup.
"""

import importlib.util
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from content import CREATE_NEW, Content  # noqa: E402

# This corpus's io.py would shadow the standard library's, so it's loaded by
# path.
_spec = importlib.util.spec_from_file_location(
    'xamgen_io', os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              'io.py'))
fileio = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fileio)

TITLE_ID = 0xFFFF7E5D
USER_NONE = 0xFF
FLAGS = 0x43

MAKE_ROOT = 'xgmk'
ROOT = 'xgof'
ROOT2 = 'xgof2'
# The high bit makes it shared content under XUID 0, stored as 'ixgof'.
PACKAGE_NAME = b'\xe9xgof'
PACKAGE = ('\\Device\\Harddisk0\\Partition1\\Content\\0000000000000000\\'
           f'{TITLE_ID:08X}\\00000001\\ixgof')
FILE_SIZE = 0x20


def open_file(c, root, path, user=USER_NONE, overlapped=False, name=None):
    """`path` is a string or a Pointer to one."""
    s = c.s
    if isinstance(path, str):
        path = s.string(path, 'path')
    license = s.alloc(4, 'license')
    args = [user, c.root(root), path, FLAGS, 0, license]
    capture = [license]
    ov = None
    if overlapped:
        ov = c.overlapped()
        capture.append(ov)
    args.append(ov or 0)
    s.call('XamContentOpenFile', *args, wait=ov, capture=capture, name=name)


def read_back(c, root):
    """Opens the file the package holds through `root` and reads it."""
    s = c.s
    handle = fileio.create(
        s, f'{root}:\\file', fileio.GENERIC_READ | fileio.SYNCHRONIZE, 0,
        fileio.FILE_OPEN, fileio.FILE_SYNCHRONOUS_IO_NONALERT |
        fileio.FILE_NON_DIRECTORY_FILE)
    fileio.read(s, handle, FILE_SIZE)
    s.call('NtClose', handle, name=f'close {root} file')


def build(corpus):
    corpus.title_id = TITLE_ID

    c = Content(corpus.scenario('arguments'))
    open_file(c, ROOT, 'game:\\xg_missing', name='missing file')
    open_file(c, ROOT, 'game:\\xg_missing', overlapped=True,
              name='missing file overlapped')
    open_file(c, ROOT, 'game:\\xamgen.xex', name='not a package')
    open_file(c, ROOT, 'game:\\xamgen.xex', overlapped=True,
              name='not a package overlapped')
    c.close(ROOT, name='close nothing open')
    for root in ('xg:t', 'xg.t', ''):
        open_file(c, root, 'game:\\xg_missing', name=f'root {root!r}')

    c = Content(corpus.scenario('package'))
    data = c.data(PACKAGE_NAME)
    c.delete(0, data, name='delete first')
    c.create(0, MAKE_ROOT, data, CREATE_NEW, name='create package')
    handle = fileio.create(
        c.s, f'{MAKE_ROOT}:\\file', fileio.GENERIC_WRITE | fileio.SYNCHRONIZE,
        fileio.FILE_ATTRIBUTE_NORMAL, fileio.FILE_CREATE,
        fileio.FILE_SYNCHRONOUS_IO_NONALERT | fileio.FILE_NON_DIRECTORY_FILE)
    fileio.write(c.s, handle, FILE_SIZE)
    c.s.call('NtClose', handle, name='close written file')
    c.close(MAKE_ROOT)

    package = c.s.string(PACKAGE, 'package path')
    open_file(c, ROOT, package, name='open package')
    read_back(c, ROOT)
    open_file(c, ROOT, package, name='open again on its root')
    open_file(c, ROOT2, package, name='open on a second root')
    read_back(c, ROOT2)
    c.close(ROOT2)
    c.close(ROOT)
    c.close(ROOT, name='close again')

    open_file(c, ROOT, package, overlapped=True, name='open overlapped')
    read_back(c, ROOT)
    c.close(ROOT)

    for user in (0, 3):
        open_file(c, ROOT, package, user=user, name=f'open as user {user}')
        c.close(ROOT, name=f'close after user {user}')

    c.delete(0, data, name='delete package')
