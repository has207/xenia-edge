"""Save write corpus: a thread that exits with an async write to a save
package pending, as DEAD OR ALIVE Xtreme 2 saves its settings.

The scenario recreates a save (CREATE_ALWAYS, no profile or device transfer)
and opens a file in it unbuffered without synchronous I/O and overwriting it.
A thread of its own writes 0xC800 bytes at offset 0 and exits. Its log holds
the write's status and its status block as the call returns.

The executor waits for that thread, then captures the status block and how
many 100 ms steps the exit took after the write returned, rounded. The
thread's exit waits for its pending requests in those steps.

The scenario closes and deletes its save. The corpus has a title of its own
for it.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from content import Content  # noqa: E402

TITLE_ID = 0xFFFF7E5E
USER = 0
USER_MODE = 1
ROOT = 'xgsave'
FILE_NAME = 'xgsave'
SAVE_FLAGS = 0x32  # CREATE_ALWAYS, no profile or device transfer
DOS_DEVICES = 0xFFFFFFFD
OBJ_CASE_INSENSITIVE = 0x40
# GENERIC_WRITE, SYNCHRONIZE, FILE_READ_ATTRIBUTES
ACCESS = 0x40100080
FILE_OVERWRITE_IF = 5
# FILE_NON_DIRECTORY_FILE, FILE_NO_INTERMEDIATE_BUFFERING
OPTIONS = 0x48
WRITE_SIZE = 0xC800
EXIT_TIMEOUT_MS = 5000
# Timebase ticks in 100 ms.
STEP = 4987500
# Writer log: {status, status block, timebase} after the write returns.
L_TIMEBASE = 12


def build(corpus):
    corpus.title_id = TITLE_ID
    scenario(corpus)


def writer(s, handle, iosb, data, offset, log):
    """A thread that writes `data` through `handle`, logs the call and exits
    without waiting for the write."""
    def emit(a, r, thunk):
        a.label(r.entry)
        a.stwu(1, -0x80, 1)
        a.load32(11, handle.address)
        a.lwz(3, 0, 11)
        a.li(4, 0)
        a.li(5, 0)
        a.load32(6, iosb.address)
        a.mr(7, 6)
        a.load32(8, data.address)
        a.load32(9, WRITE_SIZE)
        a.load32(10, offset.address)
        a.call(thunk('NtWriteFile'))
        a.load32(11, log.address)
        a.stw(3, 0, 11)
        a.load32(12, iosb.address)
        a.lwz(0, 0, 12)
        a.stw(0, 4, 11)
        a.lwz(0, 4, 12)
        a.stw(0, 8, 11)
        a.mftb(0)
        a.stw(0, L_TIMEBASE, 11)
        a.sync()
        a.li(3, 0)
        a.call(thunk('ExTerminateThread'))
    return s.routine('writer', emit)


def steps_since_write(s, log):
    """Stores at r3 the 100 ms steps since the writer's write returned,
    rounded, and returns 0."""
    def emit(a, r, thunk):
        a.label(r.entry)
        a.mftb(0)
        a.load32(11, log.address)
        a.lwz(12, L_TIMEBASE, 11)
        a.subf(10, 12, 0)
        a.load32(9, STEP // 2)
        a.add(10, 10, 9)
        a.load32(9, STEP)
        a.li(8, 0)
        a.label(r.label('count'))
        a.cmplw(10, 9)
        a.blt(r.label('done'))
        a.subf(10, 9, 10)
        a.addi(8, 8, 1)
        a.b(r.label('count'))
        a.label(r.label('done'))
        a.stw(8, 0, 3)
        a.li(3, 0)
        a.blr()
    return s.routine('steps since write', emit)


def scenario(corpus):
    s = corpus.scenario('thread exit')
    # First, so it's 512-byte aligned for the unbuffered write.
    data = s.alloc(WRITE_SIZE, 'data', fill=0x5A)
    content = Content(s)
    save = content.data(FILE_NAME)
    content.delete(USER, save, name='delete leftover')
    content.create(USER, ROOT, save, SAVE_FLAGS, name='create save')

    path = f'{ROOT}:\\data.bin'
    text = s.string(path, 'path')
    name_string = s.alloc(8, 'name', struct.pack(
        '>HHI', len(path), len(path) + 1, text.address))
    attributes = s.alloc(0xC, 'attributes', struct.pack(
        '>III', DOS_DEVICES, name_string.address, OBJ_CASE_INSENSITIVE))
    handle_out = s.alloc(4, 'handle')
    open_iosb = s.alloc(8, 'open iosb')
    handle = s.variable('handle')
    s.call('NtCreateFile', handle_out, ACCESS, attributes, open_iosb, 0, 0, 0,
           FILE_OVERWRITE_IF, OPTIONS, capture=[open_iosb],
           store=[(handle, handle_out)], name='open')

    iosb = s.alloc(8, 'write iosb', fill=0xCD)
    offset = s.alloc(8, 'offset', fill=0)
    log = s.alloc(16, 'writer log', fill=0)
    write = writer(s, handle, iosb, data, offset, log)
    writer_out = s.alloc(4, 'writer handle')
    writer_handle = s.variable('writer')
    s.call('ExCreateThread', writer_out, 0, 0, 0, write, 0, 0,
           store=[(writer_handle, writer_out)], name='create writer')
    timeout = s.alloc(8, 'exit timeout',
                      struct.pack('>q', -EXIT_TIMEOUT_MS * 10000))
    s.call('NtWaitForSingleObjectEx', writer_handle, USER_MODE, 0, timeout,
           capture=[iosb], name='wait for writer exit')
    steps = s.alloc(4, 'exit steps', fill=0)
    s.call(steps_since_write(s, log), steps,
           capture=[log.prefix(L_TIMEBASE), steps], name='exit steps')
    s.call('NtClose', writer_handle, name='close writer')

    s.call('NtClose', handle, name='close file')
    content.close(ROOT, name='close save')
    content.delete(USER, save, name='delete save')
