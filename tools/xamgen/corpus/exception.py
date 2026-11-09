"""Exception corpus: what the kernel hands a title's frame-based exception
handler for an access violation, and where the thread goes after it.

A probe is a routine with a .pdata entry naming a language handler. It logs
its stack pointers and the address of its recovery label, sets r31 to a
marker and makes the access. The handler logs the EXCEPTION_RECORD, the
DISPATCHER_CONTEXT and parts of the CONTEXT for its first call, then points
the CONTEXT at the recovery label on the probe's own stack pointer and
continues execution. The probe returns 1 when it was recovered and 0 when the
access went through. A handler called more than CALL_LIMIT times, as it is
when continuing doesn't move the thread off the fault, searches on instead,
so the fault ends the run rather than repeating. A call while unwinding is
only counted, and searches on.

Stack addresses differ between runs, so stack pointers are logged as
differences: the establisher frame from the sp the probe was entered on, the
CONTEXT's r1 from the probe's sp after its prolog. The probe saves r14-r31 in
its prolog and its epilog loads them back, so the executor's registers
survive whatever the CONTEXT held when execution continued.

The corpus runs unwatched. With an XBDM notification session open, the
console reported the first probe's fault as a first-chance exception 92 times
in 35 ms, past the handler's call limit, so something other than the handler
kept continuing it. With no session the handler was called once.
"""

import struct

FRAME = 0x100
MARKER = 0x31313131
CALL_LIMIT = 8
# Flags of an EXCEPTION_RECORD being unwound, as __C_specific_handler tests.
EXCEPTION_UNWIND = 0x66

NULL = 0x00000000
UNMAPPED = 0x00001000
# Inside the indirection range Xenia gives every address from 0x80000000,
# with no code.
NO_CODE = 0x9F000000

MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_RELEASE = 0x8000
PAGE_READWRITE = 4
DATA_PAGE = 0x1000
# Where the console put the title's first 4 KB allocation, asked for so the
# fault addresses match.
DATA_PAGE_ADDRESS = 0x00070000
BLR = 0x4E800020

# The log a probe and its handler share.
L_CALLS = 0x00
L_ENTRY_SP = 0x04
L_BODY_SP = 0x08
L_RECOVERY = 0x0C
L_RECORDED = 0x10
L_UNWINDS = 0x58
L_UNWIND_FLAGS = 0x5C
L_LOADED = 0x60
LOG_SIZE = 0x64

# Guest EXCEPTION_RECORD, DISPATCHER_CONTEXT and CONTEXT offsets.
RECORD_CODE = 0x00
RECORD_FLAGS = 0x04
RECORD_INFORMATION = 0x14
DISPATCHER_ESTABLISHER = 0x08
DISPATCHER_CONTEXT = 0x0C
CONTEXT_IAR = 0x08
CONTEXT_GPR = 0x18


def gpr_low(n):
    return CONTEXT_GPR + 8 * n + 4


def language_handler(s, log):
    """A language handler logging its first call into `log` and continuing
    every call at the probe's recovery label."""
    def emit(a, r, thunk):
        a.label(r.entry)
        a.load32(11, log.address)
        a.lwz(12, RECORD_FLAGS, 3)
        a.andi_(0, 12, EXCEPTION_UNWIND)
        a.bne(r.label('unwinding'))
        a.lwz(12, L_CALLS, 11)
        a.addi(0, 12, 1)
        a.stw(0, L_CALLS, 11)
        a.cmpwi(12, CALL_LIMIT)
        a.bge(r.label('search on'))
        a.cmpwi(12, 0)
        a.bne(r.label('recorded'))
        out = L_RECORDED

        def copy(base, offset):
            nonlocal out
            a.lwz(0, offset, base)
            a.stw(0, out, 11)
            out += 4

        def difference(minuend_reg, subtrahend_reg):
            nonlocal out
            a.subf(0, subtrahend_reg, minuend_reg)
            a.stw(0, out, 11)
            out += 4

        # Code, flags, chained record, address, parameter count, two
        # parameters.
        for offset in range(RECORD_CODE, RECORD_INFORMATION + 8, 4):
            copy(3, offset)
        a.lwz(12, L_ENTRY_SP, 11)
        difference(4, 12)
        # Control pc and function entry, then the establisher frame and
        # context record against the arguments.
        copy(6, 0x0)
        copy(6, 0x4)
        a.lwz(12, DISPATCHER_ESTABLISHER, 6)
        difference(12, 4)
        a.lwz(12, DISPATCHER_CONTEXT, 6)
        difference(12, 5)
        # Context flags, iar, lr, r1 against the probe's sp, r3, r31.
        copy(5, 0x0)
        copy(5, CONTEXT_IAR)
        copy(5, 0xC)
        a.lwz(12, gpr_low(1), 5)
        a.lwz(10, L_BODY_SP, 11)
        difference(12, 10)
        copy(5, gpr_low(3))
        copy(5, gpr_low(31))
        assert out == L_UNWINDS
        a.sync()

        a.label(r.label('recorded'))
        a.lwz(0, L_RECOVERY, 11)
        a.stw(0, CONTEXT_IAR, 5)
        a.li(0, 0)
        a.stw(0, CONTEXT_GPR + 8, 5)
        a.lwz(0, L_BODY_SP, 11)
        a.stw(0, gpr_low(1), 5)
        a.li(3, 0)                  # ExceptionContinueExecution
        a.blr()

        a.label(r.label('unwinding'))
        a.lwz(10, L_UNWINDS, 11)
        a.addi(0, 10, 1)
        a.stw(0, L_UNWINDS, 11)
        a.cmpwi(10, 0)
        a.bne(r.label('search on'))
        a.stw(12, L_UNWIND_FLAGS, 11)
        a.label(r.label('search on'))
        a.li(3, 1)                  # ExceptionContinueSearch
        a.blr()
    return s.routine('language handler', emit)


def prolog(a, r, handler):
    """The handler words, then a prolog saving lr and r14-r31."""
    a.address_word(handler.entry)
    a.word(0)                       # handler data
    a.label(r.entry)
    a.mflr(12)
    a.stw(12, -8, 1)
    for n in range(14, 32):
        a.std(n, -0x10 - 8 * (31 - n), 1)
    a.stwu(1, -FRAME, 1)
    a.label(r.label('prolog end'))


def epilog(a):
    a.addi(1, 1, FRAME)
    a.lwz(12, -8, 1)
    a.mtlr(12)
    for n in range(14, 32):
        a.ld(n, -0x10 - 8 * (31 - n), 1)
    a.blr()


def setup(a, r, log, barrier=True):
    """Logs the probe's stack pointers and recovery label and sets the
    marker, leaving the log in r11. A `barrier` ends it with a sync."""
    a.load32(11, log.address)
    a.addi(12, 1, FRAME)
    a.stw(12, L_ENTRY_SP, 11)
    a.stw(1, L_BODY_SP, 11)
    a.load_address(12, r.label('recovery'))
    a.stw(12, L_RECOVERY, 11)
    a.load32(31, MARKER)
    if barrier:
        a.sync()


def probe(s, name, log, handler, access, callee=None):
    """A framed probe whose handler is `handler`. `access(a)` makes the
    access with the address in r3 and the log in r11. A `callee(a)` is a
    frameless leaf of its own that the probe calls to make it instead."""
    def emit(a, r, thunk):
        prolog(a, r, handler)
        setup(a, r, log)
        if callee:
            a.bl(r.label('callee'))
        else:
            access(a)
        a.li(3, 0)
        a.b(r.label('epilog'))
        a.label(r.label('recovery'))
        a.li(3, 1)
        a.label(r.label('epilog'))
        epilog(a)
        a.label(r.label('end'))
        r.function(r.entry, r.label('prolog end'), r.label('end'),
                   handler=True)
        if callee:
            a.label(r.label('callee'))
            callee(a)
            a.blr()
            a.label(r.label('callee end'))
            r.function(r.label('callee'), r.label('callee'),
                       r.label('callee end'))
    return s.routine(name, emit)


def single_block_probe(s, name, log, handler, access):
    """A probe whose prolog, access and epilog are one block: no branch, call
    or sync until its return. The recovery label has an epilog of its own."""
    def emit(a, r, thunk):
        prolog(a, r, handler)
        setup(a, r, log, barrier=False)
        access(a)
        a.li(3, 0)
        epilog(a)
        a.label(r.label('recovery'))
        a.li(3, 1)
        epilog(a)
        a.label(r.label('end'))
        r.function(r.entry, r.label('prolog end'), r.label('end'),
                   handler=True)
    return s.routine(name, emit)


def captures(log):
    return [log.prefix(4), (log + L_RECORDED).prefix(L_UNWINDS - L_RECORDED),
            (log + L_UNWINDS).prefix(LOG_SIZE - L_UNWINDS)]


def fault_scenario(corpus, name, address, access=None, callee=None,
                   make=probe):
    s = corpus.scenario(name)
    log = s.alloc(LOG_SIZE, 'log', fill=0)
    handler = language_handler(s, log)
    if callee:
        routine = make(s, f'{name} probe', log, handler, None, callee)
    else:
        routine = make(s, f'{name} probe', log, handler, access)
    s.call(routine, address, name=name, capture=captures(log))


def load_used(a):
    a.lwz(4, 0, 3)
    a.stw(4, L_LOADED, 11)


def call_target(a):
    a.mtctr(3)
    a.bctrl()


def build(corpus):
    corpus.unwatched = True
    fault_scenario(corpus, 'load fault', NULL, load_used)
    fault_scenario(corpus, 'store fault', NULL, lambda a: a.stw(31, 0, 3))
    # The probe's block writes r3 again before anything reads what the load
    # put there.
    fault_scenario(corpus, 'overwritten load fault', NULL,
                   lambda a: a.lwz(3, 0, 3))
    # The handler's frame is the caller of the function that faults, which
    # it is suspended in at a call.
    fault_scenario(corpus, 'callee fault', NULL, callee=load_used)
    fault_scenario(corpus, 'single block fault', NULL, load_used,
                   make=single_block_probe)

    # A call to an address with no code faults on the instruction fetch.
    for name, address in (('call null', NULL), ('call unmapped', UNMAPPED),
                          ('call no code', NO_CODE)):
        fault_scenario(corpus, name, address, call_target)

    # The data the CLR calls as precodes is in pages it allocated.
    s = corpus.scenario('call data page')
    log = s.alloc(LOG_SIZE, 'log', fill=0)
    base = s.alloc(4, 'base', struct.pack('>I', DATA_PAGE_ADDRESS))
    size = s.alloc(4, 'size', struct.pack('>I', DATA_PAGE))
    page = s.variable('data page')
    s.call('NtAllocateVirtualMemory', base, size, MEM_RESERVE | MEM_COMMIT,
           PAGE_READWRITE, 0, name='allocate', capture=[base],
           store=[(page, base)])
    # Returns at once if it runs after all.
    s.call('RtlFillMemoryUlong', page, DATA_PAGE, BLR, name='fill',
           void=True)
    handler = language_handler(s, log)
    routine = probe(s, 'call data page probe', log, handler, call_target)
    s.call(routine, page, name='call data page', capture=captures(log))
    s.write(size, struct.pack('>I', 0))
    s.call('NtFreeVirtualMemory', base, size, MEM_RELEASE, 0, name='release')
