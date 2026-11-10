#!/usr/bin/env python3
"""Records how system exports behave by running a corpus of calls in a title.

A corpus is a list of scenarios, each a list of export calls whose pointer
arguments point into an arena the scenario lays out. xamgen builds one XEX
with a fixed executor and the corpus in its data pages. The executor runs the
steps in order: it applies each step's arena patches, loads its arguments (the
first eight into r3-r10, the rest into the stack slots from r1 + 0x50), calls
the export through its import thunk, keeps r3, optionally waits for an
XOVERLAPPED in the arena to stop pending, copies the arena ranges the step
captures and saves r3, or the word or doubleword at an arena offset, into
variables later steps can pass, as they are or plus a delta.

A recorder is a routine a step passes where the system takes a callback, such
as an APC's kernel routine. Each call counts itself in the recorder's arena
log and, for the first calls it has room for, logs r3 onward, then returns its
result, 0 by default. Its address can also be written into the arena, for a
structure the system calls through.

A routine is guest code the corpus writes, which a step calls in place of an
export. It can declare functions with .pdata entries, which the build puts in
the image's exception directory, for the kernel's unwinder to find them and
their exception handlers.

When the steps are done it writes the results to cache:\\xamgen.bin, or to
game:\\xamgen.bin where cache: isn't mounted (a console title without a utility
drive). It prints a summary with DbgPrint and stores DONE_MAGIC. Once the file
is written it ends after a second, otherwise it waits until the host writes
EXIT_MAGIC to the exit word or 30 s pass, so XBDM can read the results from
memory. The executor of an unwatched corpus always waits for the host. It
ends the title with XamLoaderTerminateTitle, which takes a console back to its
dashboard.

    python3 tools/xamgen/xamgen.py build CORPUS.py OUTDIR
    python3 tools/xamgen/xamgen.py decode OUTDIR/xamgen.json RESULTS.bin

A corpus file defines `build(corpus)` and adds scenarios to it. The build
writes OUTDIR/xamgen.xex and OUTDIR/xamgen.json, which decode needs.
"""

import argparse
import importlib.util
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))), 'xex'))
import xexbuild  # noqa: E402
from ppcasm import Assembler  # noqa: E402

LOAD_ADDRESS = 0x82000000
PAGE_SIZE = xexbuild.PAGE_SIZE
CODE_BASE = LOAD_ADDRESS + PAGE_SIZE
IMPORTS_BASE = CODE_BASE + 0xC000
CONTROL = CODE_BASE + PAGE_SIZE
ARENA = CONTROL + PAGE_SIZE
ARENA_SIZE = PAGE_SIZE
CORPUS = ARENA + PAGE_SIZE
TITLE_ID = 0xFFFF7E5B

DONE_MAGIC = 0x600DF00D
EXIT_MAGIC = 0xE817E817
CORPUS_MAGIC = 0x5847454E   # XGEN
RESULTS_MAGIC = 0x58475253  # XGRS
VERSION = 1
MAX_ARGS = 16
VARIABLES = 64
PENDING = 0x3E5
WAIT_STEPS = 1000           # 10 ms each

# Control page layout.
C_DONE = 0x000
C_EXIT = 0x004
C_PROGRESS = 0x008
C_FILE_STATUS = 0x00C
C_FIRST_SLEEP = 0x010
C_POLL_SLEEP = 0x018
C_EXIT_SLEEP = 0x020
C_HANDLE = 0x028
C_IOSB = 0x030
C_OFFSET = 0x038
C_ATTRIBUTES = 0x040
C_CACHE_NAME = 0x050
C_GAME_NAME = 0x058
C_CACHE_PATH = 0x060
C_GAME_PATH = 0x080
C_ARGS = 0x100
C_VARIABLES = 0x200
C_FORMAT = 0x400
C_STEP_FORMAT = 0x440
# The routines' function entries, the exception directory.
C_PDATA = 0x1000

CACHE_PATH = b'cache:\\xamgen.bin'
GAME_PATH = b'game:\\xamgen.bin'
FORMAT = b'xamgen: %d steps done, results file %08X\n\0'
STEP_FORMAT = b'xamgen step %d\n\0'

TAG_IMMEDIATE = 0
TAG_ARENA = 1
TAG_VARIABLE = 2
NO_WAIT = 0xFFFFFFFF
FROM_R3 = 0xFFFFFFFF

TABLES = {
    xexbuild.XAM: 'src/xenia/kernel/xam/xam_table.inc',
    xexbuild.XBOXKRNL: 'src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc',
}
REPO = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))


def load_exports():
    """Export name -> (library, ordinal), from Xenia's export tables."""
    exports = {}
    pattern = re.compile(r'XE_EXPORT\(\w+,\s*0x([0-9A-Fa-f]+),\s*(\w+),')
    for library, path in TABLES.items():
        with open(os.path.join(REPO, path)) as f:
            for match in pattern.finditer(f.read()):
                exports.setdefault(match.group(2),
                                   (library, int(match.group(1), 16)))
    return exports


# Executor.

def executor(asm, thunk, wait_for_host):
    """Emits the executor. `thunk(name)` gives an import thunk address.
    With `wait_for_host` it waits for EXIT_MAGIC even once the results file
    is written."""
    a = asm
    a.stwu(1, -0x200, 1)
    a.load32(31, CONTROL)
    # Give a host time to attach to XBDM's debug output after the launch.
    a.li(3, 1)
    a.li(4, 0)
    a.addi(5, 31, C_FIRST_SLEEP)
    a.call(thunk('KeDelayExecutionThread'))

    a.load32(4, CORPUS)
    a.lwz(25, 8, 4)                 # step count
    a.lwz(20, 0xC, 4)               # results base
    a.lwz(3, 0x14, 4)               # thunk count
    a.addi(24, 4, 0x18)             # thunk table
    a.slwi(3, 3, 2)
    a.add(30, 24, 3)                # first step
    a.load32(29, ARENA)
    a.addi(27, 31, C_VARIABLES)
    a.load32(3, RESULTS_MAGIC)
    a.stw(3, 0, 20)
    a.li(3, VERSION)
    a.stw(3, 4, 20)
    a.addi(28, 20, 0x10)
    a.li(26, 0)

    a.label('step')
    a.cmpw(26, 25)
    a.beq('steps_done')
    a.stw(26, C_PROGRESS, 31)
    a.sync()
    a.addi(3, 31, C_STEP_FORMAT)
    a.mr(4, 26)
    a.call(thunk('DbgPrint'))
    a.lwz(23, 0, 30)                # thunk index
    a.addi(30, 30, 4)

    # Arena patches: offset, length, bytes padded to 4.
    a.lwz(21, 0, 30)
    a.addi(30, 30, 4)
    a.label('patch')
    a.cmpwi(21, 0)
    a.beq('patches_done')
    a.lwz(4, 0, 30)
    a.lwz(5, 4, 30)
    a.addi(30, 30, 8)
    a.add(6, 29, 4)
    a.li(7, 0)
    a.label('patch_copy')
    a.cmplw(7, 5)
    a.bge('patch_copied')
    a.add(8, 30, 7)
    a.lbz(9, 0, 8)
    a.add(8, 6, 7)
    a.stb(9, 0, 8)
    a.addi(7, 7, 1)
    a.b('patch_copy')
    a.label('patch_copied')
    a.addi(5, 5, 3)
    a.clrrwi(5, 5, 2)
    a.add(30, 30, 5)
    a.addi(21, 21, -1)
    a.b('patch')
    a.label('patches_done')

    # Arguments: tag, high word, low word, into doubleword slots. A variable's
    # high word is the delta added to its value's low word.
    a.li(3, 0)
    for i in range(MAX_ARGS):
        a.std(3, C_ARGS + 8 * i, 31)
    a.lwz(21, 0, 30)
    a.addi(30, 30, 4)
    a.addi(6, 31, C_ARGS)
    a.label('arg')
    a.cmpwi(21, 0)
    a.beq('args_done')
    a.lwz(3, 0, 30)
    a.lwz(4, 4, 30)
    a.lwz(5, 8, 30)
    a.addi(30, 30, 12)
    a.cmpwi(3, TAG_ARENA)
    a.beq('arg_arena')
    a.cmpwi(3, TAG_VARIABLE)
    a.beq('arg_variable')
    a.b('arg_store')
    a.label('arg_arena')
    a.add(5, 29, 5)
    a.li(4, 0)
    a.b('arg_store')
    a.label('arg_variable')
    a.mr(7, 4)
    a.slwi(5, 5, 3)
    a.add(5, 27, 5)
    a.lwz(4, 0, 5)
    a.lwz(5, 4, 5)
    a.add(5, 5, 7)
    a.label('arg_store')
    a.stw(4, 0, 6)
    a.stw(5, 4, 6)
    a.addi(6, 6, 8)
    a.addi(21, 21, -1)
    a.b('arg')
    a.label('args_done')
    for i in range(8, MAX_ARGS):
        a.ld(0, C_ARGS + 8 * i, 31)
        a.std(0, 0x50 + 8 * (i - 8), 1)
    for i in range(8):
        a.ld(3 + i, C_ARGS + 8 * i, 31)
    a.slwi(12, 23, 2)
    a.lwzx(12, 24, 12)
    a.mtctr(12)
    a.bctrl()
    a.mr(22, 3)
    a.std(22, 0, 28)
    a.addi(28, 28, 8)

    # Wait for an XOVERLAPPED at an arena offset to stop pending.
    a.lwz(4, 0, 30)
    a.addi(30, 30, 4)
    a.cmpwi(4, -1)
    a.beq('no_wait')
    a.add(19, 29, 4)
    a.li(21, WAIT_STEPS)
    a.label('wait')
    a.lwz(3, 0, 19)
    a.cmplwi(3, PENDING)
    a.bne('wait_completed')
    a.cmpwi(21, 0)
    a.beq('waited')
    a.li(3, 1)
    a.li(4, 0)
    a.addi(5, 31, C_POLL_SLEEP)
    a.call(thunk('KeDelayExecutionThread'))
    a.addi(21, 21, -1)
    a.b('wait')
    # One more than the polls left, so a completion during the last sleep
    # doesn't read as a timeout.
    a.label('wait_completed')
    a.addi(21, 21, 1)
    a.label('waited')
    a.stw(21, 0, 28)
    a.b('wait_recorded')
    a.label('no_wait')
    a.li(3, -1)
    a.stw(3, 0, 28)
    a.label('wait_recorded')
    a.li(3, 0)
    a.stw(3, 4, 28)
    a.addi(28, 28, 8)

    # Captures: arena offset and length, each padded to 8 in the results.
    a.lwz(21, 0, 30)
    a.addi(30, 30, 4)
    a.label('capture')
    a.cmpwi(21, 0)
    a.beq('captures_done')
    a.lwz(4, 0, 30)
    a.lwz(5, 4, 30)
    a.addi(30, 30, 8)
    a.add(6, 29, 4)
    a.li(7, 0)
    a.label('capture_copy')
    a.cmplw(7, 5)
    a.bge('capture_copied')
    a.add(8, 6, 7)
    a.lbz(9, 0, 8)
    a.add(8, 28, 7)
    a.stb(9, 0, 8)
    a.addi(7, 7, 1)
    a.b('capture_copy')
    a.label('capture_copied')
    a.addi(5, 5, 7)
    a.clrrwi(5, 5, 3)
    a.add(28, 28, 5)
    a.addi(21, 21, -1)
    a.b('capture')
    a.label('captures_done')

    # Variables: index, source (r3 or an arena offset) and the size read from
    # the arena, a word or a doubleword.
    a.lwz(21, 0, 30)
    a.addi(30, 30, 4)
    a.label('store')
    a.cmpwi(21, 0)
    a.beq('stores_done')
    a.lwz(4, 0, 30)
    a.lwz(5, 4, 30)
    a.lwz(8, 8, 30)
    a.addi(30, 30, 12)
    a.slwi(4, 4, 3)
    a.add(4, 27, 4)
    a.cmpwi(5, -1)
    a.bne('store_arena')
    a.std(22, 0, 4)
    a.b('stored')
    a.label('store_arena')
    a.add(5, 29, 5)
    a.cmpwi(8, 8)
    a.bne('store_word')
    a.ld(6, 0, 5)
    a.std(6, 0, 4)
    a.b('stored')
    a.label('store_word')
    a.lwz(6, 0, 5)
    a.li(7, 0)
    a.stw(7, 0, 4)
    a.stw(6, 4, 4)
    a.label('stored')
    a.addi(21, 21, -1)
    a.b('store')
    a.label('stores_done')
    a.addi(26, 26, 1)
    a.b('step')

    a.label('steps_done')
    # Past the last step, so a fault from here on names none.
    a.stw(26, C_PROGRESS, 31)
    a.stw(26, 8, 20)
    a.subf(3, 20, 28)
    a.stw(3, 0xC, 20)
    write_results(a, thunk, C_CACHE_NAME, 'cache')
    a.cmpwi(3, 0)
    a.beq('written')
    write_results(a, thunk, C_GAME_NAME, 'game')
    a.label('written')
    a.stw(3, C_FILE_STATUS, 31)
    a.mr(5, 3)
    a.mr(4, 26)
    a.addi(3, 31, C_FORMAT)
    a.call(thunk('DbgPrint'))
    a.load32(3, DONE_MAGIC)
    a.sync()
    a.stw(3, C_DONE, 31)
    a.sync()

    # A second once the file is written, else up to 30 s for XBDM.
    a.load32(19, EXIT_MAGIC)
    if not wait_for_host:
        a.lwz(3, C_FILE_STATUS, 31)
        a.li(21, 10)
        a.cmpwi(3, 0)
        a.beq('exit_wait')
    a.li(21, 300)
    a.label('exit_wait')
    a.lwz(3, C_EXIT, 31)
    a.cmpw(3, 19)
    a.beq('exit')
    a.cmpwi(21, 0)
    a.beq('exit')
    a.li(3, 1)
    a.li(4, 0)
    a.addi(5, 31, C_EXIT_SLEEP)
    a.call(thunk('KeDelayExecutionThread'))
    a.addi(21, 21, -1)
    a.b('exit_wait')
    a.label('exit')
    a.call(thunk('XamLoaderTerminateTitle'))
    a.label('hang')
    a.b('hang')


def write_results(a, thunk, name_offset, suffix):
    """Writes the results to the path whose ANSI_STRING is at `name_offset`
    in the control page, leaving the status in r3."""
    a.li(3, -3)                     # ObDosDevices
    a.stw(3, C_ATTRIBUTES, 31)
    a.addi(3, 31, name_offset)
    a.stw(3, C_ATTRIBUTES + 4, 31)
    a.li(3, 0x40)                   # OBJ_CASE_INSENSITIVE
    a.stw(3, C_ATTRIBUTES + 8, 31)
    a.li(3, 0)
    a.stw(3, 0x50, 1)
    a.li(3, 0x60)                   # synchronous, not a directory
    a.stw(3, 0x54, 1)
    a.addi(3, 31, C_HANDLE)
    a.lis(4, 0x4010)                # GENERIC_WRITE | SYNCHRONIZE
    a.addi(5, 31, C_ATTRIBUTES)
    a.addi(6, 31, C_IOSB)
    a.li(7, 0)
    a.li(8, 0x80)                   # FILE_ATTRIBUTE_NORMAL
    a.li(9, 0)
    a.li(10, 5)                     # FILE_OVERWRITE_IF
    a.call(thunk('NtCreateFile'))
    a.cmpwi(3, 0)
    a.bne(f'write_{suffix}_done')
    a.lwz(3, C_HANDLE, 31)
    a.li(4, 0)
    a.li(5, 0)
    a.li(6, 0)
    a.addi(7, 31, C_IOSB)
    a.mr(8, 20)
    a.lwz(9, 0xC, 20)
    a.addi(10, 31, C_OFFSET)
    a.call(thunk('NtWriteFile'))
    a.mr(19, 3)
    a.lwz(3, C_HANDLE, 31)
    a.call(thunk('NtClose'))
    a.mr(3, 19)
    a.label(f'write_{suffix}_done')


def recorder_routine(asm, recorder, index):
    """Emits `recorder`'s routine, a leaf that keeps to volatile registers."""
    a = asm
    recorder.address = a.address()
    a.load32(11, recorder.log.address)
    a.lwz(12, 0, 11)
    a.addi(0, 12, 1)
    a.stw(0, 0, 11)
    a.cmplwi(12, recorder.calls)
    a.bge(f'recorder_{index}_full')
    a.mulli(12, 12, 8 * recorder.args)
    a.add(12, 12, 11)
    for i in range(recorder.args):
        a.std(3 + i, 8 + 8 * i, 12)
    a.sync()
    a.label(f'recorder_{index}_full')
    result = recorder.result
    if isinstance(result, Pointer):
        a.load32(3, result.address)
    elif result:
        a.load32(3, result)
    else:
        a.li(3, 0)
    a.blr()


def function_entries(asm, routines):
    """The routines' .pdata entries, sorted by start: start address, then
    prolog length, length in instructions and the 32-bit flag, with the top
    bit set for a function with an exception handler."""
    entries = []
    for routine in routines:
        for begin, prolog_end, end, handler in routine.functions:
            start = asm.label_address(begin)
            prolog = (asm.label_address(prolog_end) - start) // 4
            length = (asm.label_address(end) - start) // 4
            if not 0 <= prolog <= 0xFF or not prolog <= length < 1 << 22:
                raise ValueError(f'{routine.name}: bad function {begin}')
            entries.append((start, 0x40000000 | (handler << 31) |
                            (length << 8) | prolog))
    entries.sort()
    for (start, packed), (next_start, _) in zip(entries, entries[1:]):
        if start + 4 * ((packed >> 8) & 0x3FFFFF) > next_start:
            raise ValueError(f'functions at {start:08X} and {next_start:08X} '
                             f'overlap')
    if C_PDATA + 8 * len(entries) > PAGE_SIZE:
        raise ValueError('too many functions')
    return b''.join(struct.pack('>II', *entry) for entry in entries)


def control_page(pdata):
    page = bytearray(C_STEP_FORMAT + len(STEP_FORMAT))
    struct.pack_into('>q', page, C_FIRST_SLEEP, -3000 * 10000)
    struct.pack_into('>q', page, C_POLL_SLEEP, -10 * 10000)
    struct.pack_into('>q', page, C_EXIT_SLEEP, -100 * 10000)
    for name, path, offset in ((C_CACHE_NAME, CACHE_PATH, C_CACHE_PATH),
                               (C_GAME_NAME, GAME_PATH, C_GAME_PATH)):
        struct.pack_into('>HHI', page, name, len(path), len(path) + 1,
                         CONTROL + offset)
        page[offset:offset + len(path)] = path
    page[C_FORMAT:C_FORMAT + len(FORMAT)] = FORMAT
    page[C_STEP_FORMAT:] = STEP_FORMAT
    if pdata:
        page += bytes(C_PDATA - len(page)) + pdata
    return bytes(page)


# Corpus.

class Pointer:
    """An arena range a scenario lays out."""

    def __init__(self, offset, size, name):
        self.offset = offset
        self.size = size
        self.name = name

    def __add__(self, delta):
        return Pointer(self.offset + delta, self.size - delta,
                       f'{self.name}+{delta:#x}')

    def prefix(self, size):
        """The range's first `size` bytes."""
        return Pointer(self.offset, size, f'{self.name}[:{size:#x}]')

    @property
    def address(self):
        """The guest address, for a pointer stored inside arena data."""
        return ARENA + self.offset


class Variable:
    def __init__(self, index, name, delta=0):
        self.index = index
        self.name = name
        self.delta = delta

    def __add__(self, delta):
        """The value plus `delta` in its low word, as a call argument."""
        return Variable(self.index, self.name, self.delta + delta)

    @property
    def address(self):
        """The guest address of the value's low word, for an export that
        reads it from memory."""
        return CONTROL + C_VARIABLES + 8 * self.index + 4


class Recorder:
    """A callback whose `log` holds its call count and then `args`
    doublewords from r3 for each of its first `calls` calls. It returns
    `result`, a value or a Pointer's address."""

    def __init__(self, log, args, calls, result):
        self.log = log
        self.args = args
        self.calls = calls
        self.result = result
        self.address = None


class Routine:
    """Guest code a step calls like an export. `emit(asm, routine, thunk)`
    writes it after the executor, placing `entry` where calls start, naming
    its labels with label() and declaring its functions with function().
    `thunk(name)` gives an export's import thunk."""

    def __init__(self, name, prefix, emit):
        self.name = name
        self.prefix = prefix
        self.emit = emit
        self.entry = self.label('entry')
        self.functions = []
        self.address = None

    def label(self, name):
        """The assembler label `name` of this routine."""
        return f'{self.prefix}.{name}'

    def function(self, begin, prolog_end, end, handler=False):
        """Declares a .pdata entry for the function from label `begin` to
        `end`, its prolog ending at `prolog_end`. A function with a `handler`
        keeps its handler and the handler's data in the two words before
        `begin`."""
        self.functions.append((begin, prolog_end, end, handler))


class Scenario:
    def __init__(self, corpus, name, fill, console_only):
        self.corpus = corpus
        self.name = name
        self.fill = fill
        self.console_only = console_only
        self.used = 0
        self.initial = bytearray()
        self.pending = []
        self.steps = []
        self.recorders = []
        self.routines = []

    def alloc(self, size, name, data=None, fill=None):
        """Lays out `size` bytes, 8-aligned, holding `data` and then `fill`
        (the scenario's by default)."""
        offset = (self.used + 7) & ~7
        if offset + size > ARENA_SIZE:
            raise ValueError(f'{self.name}: arena full')
        if data and len(data) > size:
            raise ValueError(f'{self.name}: {name} data past its size')
        self.used = offset + size
        image = bytearray([self.fill if fill is None else fill] * size)
        if data:
            image[:len(data)] = data
        self.initial += bytes([self.fill] * (offset - len(self.initial)))
        self.initial += image
        return Pointer(offset, size, name)

    def string(self, text, name='string', size=None):
        data = text.encode() + b'\0' if isinstance(text, str) else text
        return self.alloc(size or len(data), name, data, fill=0)

    def write(self, pointer, data):
        """Writes `data` at `pointer` before the next call, bytes or a
        Recorder's address."""
        if isinstance(data, Recorder):
            if pointer.size < 4:
                raise ValueError(f'{self.name}: write past {pointer.name}')
            self.pending.append((pointer.offset, data))
            return
        if len(data) > pointer.size:
            raise ValueError(f'{self.name}: write past {pointer.name}')
        self.pending.append((pointer.offset, bytes(data)))

    def variable(self, name):
        return self.corpus.variable(name)

    def recorder(self, name, args=1, calls=1, result=0):
        """A Recorder for up to 8 arguments, its log zeroed."""
        if not 1 <= args <= 8:
            raise ValueError(f'{self.name}: {name} records 1 to 8 arguments')
        log = self.alloc(8 + 8 * args * calls, name, fill=0)
        recorder = Recorder(log, args, calls, result)
        self.recorders.append(recorder)
        return recorder

    def routine(self, name, emit):
        """A Routine of this scenario, which `emit` writes."""
        routine = Routine(name, f'routine{self.corpus.routine_count}', emit)
        self.corpus.routine_count += 1
        self.routines.append(routine)
        return routine

    def call(self, export, *args, name=None, wait=None, capture=(),
             store=(), faults=False, void=False):
        """Calls `export`, an export's name or a Routine, with `args`: ints,
        Pointers, Variables, Recorders or Routines, the last two by address.
        `wait` is an XOVERLAPPED Pointer, `capture` the Pointers to record and
        `store` (Variable, 'r3' or Pointer) pairs, an 8-byte
        Pointer saving a doubleword and any other its first word. A step that
        `faults` on a console is recorded as such and never run. A `void`
        export's r3 is whatever it left there, so it isn't recorded."""
        if len(args) > MAX_ARGS:
            raise ValueError(f'{export}: too many arguments')
        if name is None:
            name = export.name if isinstance(export, Routine) else export
        self.steps.append({
            'export': export, 'args': list(args), 'name': name,
            'wait': wait, 'capture': list(capture), 'store': list(store),
            'patches': self.pending, 'faults': faults, 'void': void})
        if not faults:
            self.pending = []


class Corpus:
    def __init__(self, name):
        self.name = name
        # A corpus whose leftovers would show in another's runs takes a title
        # of its own.
        self.title_id = TITLE_ID
        self.scenarios = []
        self.variables = {}
        self.exports = load_exports()
        self.routine_count = 0
        # A console runs an unwatched corpus with no XBDM notification
        # session, since XBDM with one open raises a first-chance exception
        # again and again instead of dispatching it to the title's handlers.
        self.unwatched = False

    def scenario(self, name, fill=0xCD, console_only=None):
        """`console_only` says why Xenia mustn't run the scenario, such as a
        call that would damage the host, and run.py refuses a build with it."""
        scenario = Scenario(self, name, fill, console_only)
        self.scenarios.append(scenario)
        return scenario

    def variable(self, name):
        if name not in self.variables:
            if len(self.variables) == VARIABLES:
                raise ValueError('out of variables')
            self.variables[name] = Variable(len(self.variables), name)
        return self.variables[name]


def describe(value):
    if isinstance(value, Pointer):
        return {'pointer': value.name, 'offset': value.offset}
    if isinstance(value, Variable):
        if value.delta:
            return {'variable': value.name, 'delta': value.delta}
        return {'variable': value.name}
    if isinstance(value, Recorder):
        return {'recorder': value.log.name}
    if isinstance(value, Routine):
        return {'routine': value.name}
    return {'value': value & 0xFFFFFFFFFFFFFFFF}


def compile_corpus(corpus, imports):
    """The corpus blob, the results size and the step descriptions."""
    thunks = []
    thunk_index = {}
    blob = bytearray()
    steps = []
    results_size = 0x10
    for scenario in corpus.scenarios:
        first = True
        for step in scenario.steps:
            target = step['export']
            if isinstance(target, Routine):
                export = target.name
            elif target in corpus.exports:
                export = target
            else:
                raise ValueError(f'unknown export {target}')
            description = {
                'scenario': scenario.name, 'name': step['name'],
                'export': export, 'args': [describe(a) for a in step['args']]}
            if step['faults']:
                steps.append(dict(description, faults=True))
                continue
            if target not in thunk_index:
                thunk_index[target] = len(thunks)
                thunks.append(target.address if isinstance(target, Routine)
                              else imports.function(*corpus.exports[target]))
            record = bytearray(struct.pack('>I', thunk_index[target]))
            patches = list(step['patches'])
            if first:
                patches.insert(0, (0, bytes(scenario.initial)))
                first = False
            record += struct.pack('>I', len(patches))
            for offset, data in patches:
                if isinstance(data, Recorder):
                    data = struct.pack('>I', data.address)
                record += struct.pack('>II', offset, len(data)) + data
                record += bytes(-len(data) % 4)
            record += struct.pack('>I', len(step['args']))
            for arg in step['args']:
                if isinstance(arg, Pointer):
                    record += struct.pack('>III', TAG_ARENA, 0, arg.offset)
                elif isinstance(arg, Variable):
                    record += struct.pack('>III', TAG_VARIABLE,
                                          arg.delta & 0xFFFFFFFF, arg.index)
                elif isinstance(arg, (Recorder, Routine)):
                    record += struct.pack('>III', TAG_IMMEDIATE, 0,
                                          arg.address)
                else:
                    value = arg & 0xFFFFFFFFFFFFFFFF
                    record += struct.pack('>III', TAG_IMMEDIATE, value >> 32,
                                          value & 0xFFFFFFFF)
            wait = step['wait']
            record += struct.pack('>I', wait.offset if wait else NO_WAIT)
            record += struct.pack('>I', len(step['capture']))
            for pointer in step['capture']:
                record += struct.pack('>II', pointer.offset, pointer.size)
            record += struct.pack('>I', len(step['store']))
            for variable, source in step['store']:
                record += struct.pack(
                    '>III', variable.index,
                    FROM_R3 if source == 'r3' else source.offset,
                    0 if source == 'r3' else 8 if source.size == 8 else 4)
            blob += record
            results_size += 16 + sum((p.size + 7) & ~7
                                     for p in step['capture'])
            steps.append(dict(
                description, wait=bool(wait), void=step['void'],
                capture=[{'name': p.name, 'offset': p.offset,
                          'size': p.size} for p in step['capture']],
                store=[{'variable': v.name,
                        'source': 'r3' if s == 'r3' else s.name}
                       for v, s in step['store']]))
    return thunks, bytes(blob), results_size, steps


def build(corpus, outdir):
    imports = xexbuild.Imports(IMPORTS_BASE)
    asm = Assembler(CODE_BASE)
    exports = corpus.exports

    def thunk(name):
        return imports.function(*exports[name])

    executor(asm, thunk, corpus.unwatched)
    recorders = [r for s in corpus.scenarios for r in s.recorders]
    for index, recorder in enumerate(recorders):
        recorder_routine(asm, recorder, index)
    routines = [r for s in corpus.scenarios for r in s.routines]
    for routine in routines:
        routine.emit(asm, routine, thunk)
    text = asm.code()
    if CODE_BASE + len(text) > IMPORTS_BASE:
        raise ValueError('executor runs into the import records')
    for routine in routines:
        routine.address = asm.label_address(routine.entry)
    pdata = function_entries(asm, routines)
    thunks, blob, results_size, steps = compile_corpus(corpus, imports)
    if len(imports.data()) > CONTROL - IMPORTS_BASE:
        raise ValueError('too many imports')
    code = bytes(text) + bytes(IMPORTS_BASE - CODE_BASE - len(text))
    code += imports.data()

    header_size = 0x18 + 4 * len(thunks)
    corpus_size = header_size + len(blob)
    results = CORPUS + ((corpus_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
    header = struct.pack('>IIIIII', CORPUS_MAGIC, VERSION,
                         sum(1 for step in steps if not step.get('faults')),
                         results, results_size, len(thunks))
    header += b''.join(struct.pack('>I', t) for t in thunks)
    corpus_data = header + blob

    pages = [(xexbuild.SECTION_CODE, code),
             (xexbuild.SECTION_DATA, control_page(pdata)),
             (xexbuild.SECTION_DATA, b'')]
    for start in range(0, len(corpus_data), PAGE_SIZE):
        pages.append((xexbuild.SECTION_DATA,
                      corpus_data[start:start + PAGE_SIZE]))
    for _ in range((results_size + PAGE_SIZE - 1) // PAGE_SIZE):
        pages.append((xexbuild.SECTION_DATA, b''))
    directories = {}
    if pdata:
        directories[xexbuild.PE_EXCEPTION_DIRECTORY] = (CONTROL + C_PDATA,
                                                        len(pdata))
    xex = xexbuild.build(LOAD_ADDRESS, CODE_BASE, pages, title_id=corpus.title_id,
                         imports=imports, directories=directories)
    run = sum(1 for step in steps if not step.get('faults'))
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, 'xamgen.xex'), 'wb') as f:
        f.write(xex)
    meta = {'corpus': corpus.name, 'title_id': corpus.title_id,
            'version': VERSION, 'control': CONTROL,
            'results': results, 'results_size': results_size,
            'done_magic': DONE_MAGIC, 'exit_offset': C_EXIT,
            'exit_magic': EXIT_MAGIC, 'unwatched': corpus.unwatched,
            'steps': steps,
            'console_only': [f'{s.name}: {s.console_only}'
                             for s in corpus.scenarios
                             if s.console_only is not None]}
    with open(os.path.join(outdir, 'xamgen.json'), 'w') as f:
        json.dump(meta, f, indent=1)
    print(f'{run} of {len(steps)} steps run, {len(xex):#x} bytes, code '
          f'{len(text)} bytes, results {results_size:#x} bytes at {results:#x}')


# Results.

def decode(meta, data):
    magic, version, done, size = struct.unpack_from('>IIII', data, 0)
    if magic != RESULTS_MAGIC or version != VERSION:
        raise ValueError('not xamgen results')
    run_count = sum(1 for step in meta['steps'] if not step.get('faults'))
    if size > meta['results_size'] or size > len(data) or done > run_count:
        raise ValueError('results from another build or cut short')
    p = 0x10
    out = []
    run = 0
    for step in meta['steps']:
        if step.get('faults'):
            out.append({'scenario': step['scenario'], 'name': step['name'],
                        'export': step['export'], 'faults': True})
            continue
        if run == done:
            break
        run += 1
        r3, wait = struct.unpack_from('>QI', data, p)
        p += 16
        record = {'scenario': step['scenario'], 'name': step['name'],
                  'export': step['export']}
        if not step.get('void'):
            record['r3'] = f'{r3:016X}'
        if step['wait']:
            record['wait_left'] = wait
        captures = {}
        for capture in step['capture']:
            captures[capture['name']] = data[p:p + capture['size']].hex()
            p += (capture['size'] + 7) & ~7
        if captures:
            record['capture'] = captures
        out.append(record)
    if p != size:
        raise ValueError('results from another build or cut short')
    return {'corpus': meta['corpus'], 'steps_done': done,
            'steps_run': run_count, 'steps': len(meta['steps']),
            'results': out}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('build')
    p.add_argument('corpus')
    p.add_argument('outdir')
    p.add_argument('--only', action='append',
                   help='keep only the scenarios whose names contain this')
    p = sub.add_parser('decode')
    p.add_argument('meta')
    p.add_argument('results')
    args = parser.parse_args()
    if args.command == 'build':
        spec = importlib.util.spec_from_file_location('corpus', args.corpus)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        corpus = Corpus(os.path.splitext(os.path.basename(args.corpus))[0])
        module.build(corpus)
        if args.only:
            # Comparisons need what identity reads, whatever's kept.
            corpus.scenarios = [s for s in corpus.scenarios
                                if s.name == 'identity' or
                                any(o in s.name for o in args.only)]
        build(corpus, args.outdir)
    else:
        with open(args.meta) as f:
            meta = json.load(f)
        with open(args.results, 'rb') as f:
            data = f.read()
        json.dump(decode(meta, data), sys.stdout, indent=1)
        print()


if __name__ == '__main__':
    main()
