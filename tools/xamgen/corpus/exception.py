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

The raise probes call RtlRaiseException with an EXCEPTION_RECORD in the
arena. Left out because Xenia differs by design: the kernel takes the CONTEXT
of a raise inside RtlRaiseException, so its iar, lr and r1 and the record's
address are the kernel's, where Xenia reports the call to it. Records the
kernel makes itself are on its stack, so they are only told apart from the
corpus's own.
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
RECORD_CHAIN = 0x08
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


def save_frame(a):
    """A prolog saving lr and r14-r31 and making a FRAME-byte frame."""
    a.mflr(12)
    a.stw(12, -8, 1)
    for n in range(14, 32):
        a.std(n, -0x10 - 8 * (31 - n), 1)
    a.stwu(1, -FRAME, 1)


def prolog(a, r, handler):
    """The handler words, then a prolog saving lr and r14-r31."""
    a.address_word(handler.entry)
    a.word(0)                       # handler data
    a.label(r.entry)
    save_frame(a)
    a.label(r.label('prolog end'))


def epilog(a):
    a.addi(1, 1, FRAME)
    a.lwz(12, -8, 1)
    a.mtlr(12)
    for n in range(14, 32):
        a.ld(n, -0x10 - 8 * (31 - n), 1)
    a.blr()


def setup(a, r, log, barrier=True, marker=MARKER):
    """Logs the probe's stack pointers and recovery label and sets the
    marker, leaving the log in r11. A `barrier` ends it with a sync."""
    a.load32(11, log.address)
    a.addi(12, 1, FRAME)
    a.stw(12, L_ENTRY_SP, 11)
    a.stw(1, L_BODY_SP, 11)
    a.load_address(12, r.label('recovery'))
    a.stw(12, L_RECOVERY, 11)
    a.load32(31, marker)
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


# A scripted handler's log. Its probe's setup writes the stack pointers and
# recovery label at L_ENTRY_SP, L_BODY_SP and L_RECOVERY.
RECORD_ADDRESS = 0x0C
H_CALLS = 0x00
H_RECORDS = 0x10
# Per call: the record's code, flags, address, parameter count and first two
# parameters (0 past the count), the establisher frame against the probe's
# entry sp, the dispatcher context's control pc and function entry, its
# establisher frame and context record against the arguments, then the
# CONTEXT's iar and r3.
CALL_RECORD = 0x34
RECORD_PARAMETERS = 0x10
FAULT_R3 = 0x77
FAULT_R31 = 0x7777
LOGGED_CALLS = 3
# What the probe's recovery label resumed with: r3, r1 against the probe's
# sp, r31.
H_RESUMED = H_RECORDS + CALL_RECORD * LOGGED_CALLS
H_LOADED = H_RESUMED + 0x0C
# An RtlUnwind2 context after the unwind: flags, iar, lr, r1 against the
# probe's sp, r3, r31.
H_CONTEXT = H_LOADED + 0x04
H_SIZE = H_CONTEXT + 0x18
HANDLER_FRAME = 0x60
UNWIND_VALUE = 0x5A5A5A5A
INNER_MARKER = 0x32323232
CONTEXT_SIZE = 0xA40


def h_captures(log):
    """A scripted handler's log but for the stack addresses setup writes."""
    return [log.prefix(4), (log + H_RECORDS).prefix(H_SIZE - H_RECORDS)]


def scripted_handler(s, log, actions, context=None):
    """A framed language handler that logs its first LOGGED_CALLS calls and
    acts on call k as actions[k] says, searching on past the end: 'continue'
    at the probe's recovery label with r3 0, 'search', 'fault' on a load from
    address 0, 'unwind' with RtlUnwind, or 'unwind2' with RtlUnwind2, no
    exception record and `context`. Both unwind to the frame whose handler it
    is, at its probe's recovery label with UNWIND_VALUE."""
    def emit(a, r, thunk):
        a.label(r.entry)
        a.mflr(12)
        a.stw(12, -8, 1)
        a.std(31, -0x10, 1)
        a.stwu(1, -HANDLER_FRAME, 1)
        a.label(r.label('prolog end'))
        a.load32(11, log.address)
        a.lwz(12, H_CALLS, 11)
        a.addi(0, 12, 1)
        a.stw(0, H_CALLS, 11)
        a.cmpwi(12, LOGGED_CALLS)
        a.bge(r.label('act'))
        a.mulli(10, 12, CALL_RECORD)
        a.add(10, 10, 11)
        out = H_RECORDS

        def copy(base, offset):
            nonlocal out
            a.lwz(0, offset, base)
            a.stw(0, out, 10)
            out += 4

        def difference(minuend_reg, subtrahend_reg):
            nonlocal out
            a.subf(0, subtrahend_reg, minuend_reg)
            a.stw(0, out, 10)
            out += 4

        for offset in (RECORD_CODE, RECORD_FLAGS, RECORD_ADDRESS,
                       RECORD_PARAMETERS):
            copy(3, offset)
        for n in range(2):
            a.lwz(9, RECORD_PARAMETERS, 3)
            a.li(0, 0)
            a.cmpwi(9, n)
            a.ble(r.label(f'parameter {n} logged'))
            a.lwz(0, RECORD_INFORMATION + 4 * n, 3)
            a.label(r.label(f'parameter {n} logged'))
            a.stw(0, out, 10)
            out += 4
        a.lwz(9, L_ENTRY_SP, 11)
        difference(4, 9)
        copy(6, 0x0)
        copy(6, 0x4)
        a.lwz(9, DISPATCHER_ESTABLISHER, 6)
        difference(9, 4)
        a.lwz(9, DISPATCHER_CONTEXT, 6)
        difference(9, 5)
        copy(5, CONTEXT_IAR)
        copy(5, gpr_low(3))
        assert out == H_RECORDS + CALL_RECORD
        a.sync()

        a.label(r.label('act'))
        for k in range(len(actions)):
            a.cmpwi(12, k)
            a.beq(r.label(f'action {k}'))
        a.b(r.label('search'))
        for k, action in enumerate(actions):
            a.label(r.label(f'action {k}'))
            if action == 'continue':
                a.lwz(0, L_RECOVERY, 11)
                a.stw(0, CONTEXT_IAR, 5)
                a.li(0, 0)
                a.stw(0, CONTEXT_GPR + 8, 5)
                a.stw(0, CONTEXT_GPR + 8 * 3, 5)
                a.stw(0, gpr_low(3), 5)
                a.lwz(0, L_BODY_SP, 11)
                a.stw(0, gpr_low(1), 5)
                a.li(3, 0)              # ExceptionContinueExecution
                a.b(r.label('return'))
            elif action == 'search':
                a.b(r.label('search'))
            elif action == 'fault':
                # The nested fault's CONTEXT would show the record in r3 and
                # whatever this handler's caller left in r31.
                a.li(3, FAULT_R3)
                a.li(31, FAULT_R31)
                a.li(9, 0)
                a.label(r.label('fault'))
                a.lwz(10, 0, 9)
                a.stw(10, H_LOADED, 11)
                a.b(r.label('search'))
            elif action in ('unwind', 'unwind2'):
                a.mr(5, 3)
                a.mr(3, 4)
                a.lwz(4, L_RECOVERY, 11)
                a.load32(6, UNWIND_VALUE)
                if action == 'unwind':
                    a.call(thunk('RtlUnwind'))
                else:
                    a.li(5, 0)
                    a.load32(7, context.address)
                    a.call(thunk('RtlUnwind2'))
                a.b(r.label('search'))
            else:
                raise ValueError(f'unknown action {action}')
        a.label(r.label('search'))
        a.li(3, 1)                      # ExceptionContinueSearch
        a.label(r.label('return'))
        a.addi(1, 1, HANDLER_FRAME)
        a.lwz(12, -8, 1)
        a.mtlr(12)
        a.ld(31, -0x10, 1)
        a.blr()
        a.label(r.label('end'))
        r.function(r.entry, r.label('prolog end'), r.label('end'))
    return s.routine('scripted handler', emit)


def resumable(s, name, log, handler, body, marker=MARKER, context=None,
              extra=None):
    """A framed probe whose handler is the routine `handler`. `body(a, r,
    thunk)` runs after setup, with the address in r3 and the log in r11, and
    the probe returns 0 after it. Its recovery label logs what it resumed
    with, and what the RtlUnwind2 `context` holds then, and returns 1.
    `extra(a, r, thunk)` adds functions after it."""
    def emit(a, r, thunk):
        prolog(a, r, handler)
        setup(a, r, log, marker=marker)
        body(a, r, thunk)
        a.li(3, 0)
        a.b(r.label('epilog'))
        a.label(r.label('recovery'))
        a.load32(11, log.address)
        a.stw(3, H_RESUMED, 11)
        a.lwz(9, L_BODY_SP, 11)
        a.subf(0, 9, 1)
        a.stw(0, H_RESUMED + 4, 11)
        a.stw(31, H_RESUMED + 8, 11)
        if context:
            a.load32(10, context.address)
            for out, offset in enumerate((0x0, CONTEXT_IAR, 0xC)):
                a.lwz(0, offset, 10)
                a.stw(0, H_CONTEXT + 4 * out, 11)
            a.lwz(0, gpr_low(1), 10)
            a.subf(0, 9, 0)
            a.stw(0, H_CONTEXT + 0xC, 11)
            a.lwz(0, gpr_low(3), 10)
            a.stw(0, H_CONTEXT + 0x10, 11)
            a.lwz(0, gpr_low(31), 10)
            a.stw(0, H_CONTEXT + 0x14, 11)
        a.li(3, 1)
        a.label(r.label('epilog'))
        epilog(a)
        a.label(r.label('end'))
        r.function(r.entry, r.label('prolog end'), r.label('end'),
                   handler=True)
        if extra:
            extra(a, r, thunk)
    return s.routine(name, emit)


def load_logged(a, r, thunk):
    a.lwz(4, 0, 3)
    a.stw(4, H_LOADED, 11)


def calls(routine):
    """A body calling `routine` with the probe's arguments."""
    def body(a, r, thunk):
        a.bl(routine.entry)
    return body


def handler_scenario(corpus, name, actions, with_context=False,
                     body=load_logged):
    """A probe with a scripted handler, running `body` on address 0, with an
    RtlUnwind2 context if it unwinds with one."""
    s = corpus.scenario(name)
    log = s.alloc(H_SIZE, 'log', fill=0)
    context = None
    if with_context:
        context = s.alloc(CONTEXT_SIZE, 'unwind context', fill=0)
    handler = scripted_handler(s, log, actions, context)
    routine = resumable(s, f'{name} probe', log, handler, body,
                        context=context)
    s.call(routine, NULL, name=name, capture=h_captures(log))


def outer_scenario(corpus, name, outer_actions, inner_actions):
    """A probe with a scripted handler calling another, whose access faults,
    each with a log and marker of its own."""
    s = corpus.scenario(name)
    outer_log = s.alloc(H_SIZE, 'outer log', fill=0)
    inner_log = s.alloc(H_SIZE, 'inner log', fill=0)
    inner = resumable(s, f'{name} inner', inner_log,
                      scripted_handler(s, inner_log, inner_actions),
                      load_logged, marker=INNER_MARKER)
    outer = resumable(s, f'{name} outer', outer_log,
                      scripted_handler(s, outer_log, outer_actions),
                      calls(inner))
    s.call(outer, NULL, name=name,
           capture=h_captures(outer_log) + h_captures(inner_log))


def unwind_without_exception(corpus):
    """A probe whose callee unwinds to it with RtlUnwind2, no exception
    record and nothing faulting."""
    def body(a, r, thunk):
        a.addi(3, 1, FRAME)
        a.lwz(4, L_RECOVERY, 11)
        a.bl(r.label('unwinder'))

    def unwinder(context):
        def extra(a, r, thunk):
            a.label(r.label('unwinder'))
            save_frame(a)
            a.label(r.label('unwinder prolog end'))
            a.load32(31, INNER_MARKER)
            a.li(5, 0)
            a.load32(6, UNWIND_VALUE)
            a.load32(7, context.address)
            a.call(thunk('RtlUnwind2'))
            a.li(3, 0)
            epilog(a)
            a.label(r.label('unwinder end'))
            r.function(r.label('unwinder'), r.label('unwinder prolog end'),
                       r.label('unwinder end'))
        return extra

    s = corpus.scenario('unwind without exception')
    log = s.alloc(H_SIZE, 'log', fill=0)
    context = s.alloc(CONTEXT_SIZE, 'unwind context', fill=0)
    handler = scripted_handler(s, log, [])
    routine = resumable(s, 'unwind without exception probe', log, handler,
                        body, context=context, extra=unwinder(context))
    s.call(routine, NULL, name='unwind without exception',
           capture=h_captures(log))


def leaf_scenario(corpus, name, leaf):
    """A probe whose scripted handler continues the fault of a leaf it calls,
    which `leaf(a, r, log)` emits at the label 'leaf' with its .pdata. The
    probe logs r31 after the call."""
    s = corpus.scenario(name)
    log = s.alloc(H_SIZE, 'log', fill=0)
    handler = scripted_handler(s, log, ['continue'])

    def body(a, r, thunk):
        a.bl(r.label('leaf'))
        a.load32(11, log.address)
        a.stw(31, H_RESUMED + 8, 11)

    def extra(a, r, thunk):
        leaf(a, r, log)

    routine = resumable(s, f'{name} probe', log, handler, body, extra=extra)
    s.call(routine, NULL, name=name, capture=h_captures(log))


def leaf_saving_in_prolog(a, r, log):
    """A frameless leaf whose prolog saves r31 below its stack pointer."""
    a.label(r.label('leaf'))
    a.std(31, -0x10, 1)
    a.label(r.label('leaf prolog end'))
    a.load32(31, INNER_MARKER)
    a.lwz(4, 0, 3)
    a.stw(4, H_LOADED, 11)
    a.ld(31, -0x10, 1)
    a.blr()
    a.label(r.label('leaf end'))
    r.function(r.label('leaf'), r.label('leaf prolog end'),
               r.label('leaf end'))


def leaf_saving_in_body(a, r, log):
    """A frameless leaf saving r31 below its stack pointer, whose fault the
    handler continues at the leaf's own recovery label, which restores r31."""
    a.label(r.label('leaf'))
    a.std(31, -0x10, 1)
    a.load32(31, INNER_MARKER)
    a.load_address(12, r.label('leaf recovery'))
    a.stw(12, L_RECOVERY, 11)
    a.lwz(4, 0, 3)
    a.stw(4, H_LOADED, 11)
    a.label(r.label('leaf recovery'))
    a.ld(31, -0x10, 1)
    a.blr()
    a.label(r.label('leaf end'))
    r.function(r.label('leaf'), r.label('leaf'), r.label('leaf end'))


def faulting_prolog(a, r, log):
    """A framed function whose prolog faults after its stwu."""
    a.label(r.label('leaf'))
    a.mflr(12)
    a.stw(12, -8, 1)
    a.stwu(1, -0x60, 1)
    a.lwz(4, 0, 3)
    a.label(r.label('leaf prolog end'))
    a.stw(4, H_LOADED, 11)
    a.addi(1, 1, 0x60)
    a.lwz(12, -8, 1)
    a.mtlr(12)
    a.blr()
    a.label(r.label('leaf end'))
    r.function(r.label('leaf'), r.label('leaf prolog end'),
               r.label('leaf end'))


# A __C_specific_handler probe's log. Its setup writes the stack pointers and
# recovery label at L_ENTRY_SP, L_BODY_SP and L_RECOVERY.
FILTERS = 3
# Per filter: calls, then for its first call the record's code and address,
# the CONTEXT's iar and r12 against the probe's entry sp.
C_FILTERS = 0x10
FILTER_RECORD = 0x14
# Where the probe went on: the except label's number (0 for recovery), r3,
# r1 against the probe's sp, r31.
C_RESUMED = C_FILTERS + FILTER_RECORD * FILTERS
# The finally handler's calls, last r3 and r12 against its function's entry
# sp.
C_FINALLY = C_RESUMED + 0x10
C_LOADED = C_FINALLY + 0x0C
C_INNER_ENTRY_SP = C_LOADED + 0x04
C_SIZE = C_INNER_ENTRY_SP + 0x04


def c_captures(log):
    """A __C_specific_handler probe's log but for stack addresses."""
    return [(log + C_FILTERS).prefix(C_INNER_ENTRY_SP - C_FILTERS)]


def c_words(a, thunk, table):
    """__C_specific_handler and its scope table as the handler words."""
    a.word(thunk('__C_specific_handler'))
    a.address_word(table)


def scope_table(a, label, scopes):
    """Scopes of (begin, end, handler, target) labels, a target of None for
    a finally handler."""
    a.label(label)
    a.word(len(scopes))
    for begin, end, handler, target in scopes:
        a.address_word(begin)
        a.address_word(end)
        a.address_word(handler)
        if target:
            a.address_word(target)
        else:
            a.word(0)


def c_resumed(a, r, log, number):
    """Logs where the probe went on and returns 1 from it."""
    a.load32(11, log.address)
    a.li(0, number)
    a.stw(0, C_RESUMED, 11)
    a.stw(3, C_RESUMED + 4, 11)
    a.lwz(9, L_BODY_SP, 11)
    a.subf(0, 9, 1)
    a.stw(0, C_RESUMED + 8, 11)
    a.stw(31, C_RESUMED + 0xC, 11)
    a.li(3, 1)
    a.b(r.label('epilog'))


def c_filter(a, r, log, index, result, continue_at_recovery=False):
    """A filter that logs its first call and returns `result`, pointing the
    CONTEXT at the probe's recovery label first if asked."""
    base = C_FILTERS + FILTER_RECORD * index
    a.label(r.label(f'filter {index}'))
    a.load32(11, log.address)
    a.lwz(10, base, 11)
    a.addi(0, 10, 1)
    a.stw(0, base, 11)
    a.cmpwi(10, 0)
    a.bne(r.label(f'filter {index} result'))
    a.lwz(9, 0, 3)                      # ExceptionRecord
    a.lwz(0, RECORD_CODE, 9)
    a.stw(0, base + 4, 11)
    a.lwz(0, RECORD_ADDRESS, 9)
    a.stw(0, base + 8, 11)
    a.lwz(9, 4, 3)                      # ContextRecord
    a.lwz(0, CONTEXT_IAR, 9)
    a.stw(0, base + 0xC, 11)
    a.lwz(10, L_ENTRY_SP, 11)
    a.subf(0, 10, 12)
    a.stw(0, base + 0x10, 11)
    if continue_at_recovery:
        a.lwz(0, L_RECOVERY, 11)
        a.stw(0, CONTEXT_IAR, 9)
        a.li(0, 0)
        a.stw(0, CONTEXT_GPR + 8, 9)
        a.lwz(0, L_BODY_SP, 11)
        a.stw(0, gpr_low(1), 9)
    a.label(r.label(f'filter {index} result'))
    a.li(3, result)
    a.blr()


def c_scenario(corpus, name, body, scopes, filters, extra=None):
    """A probe with __C_specific_handler as its handler, run on address 0.
    `body(a, r)` runs after setup and places the labels `scopes` name, which
    can also name 'except N' labels that log and return 1. `filters` are
    (result, continue_at_recovery) per filter."""
    s = corpus.scenario(name)
    log = s.alloc(C_SIZE, 'log', fill=0)

    def emit(a, r, thunk):
        c_words(a, thunk, r.label('scopes'))
        a.label(r.entry)
        save_frame(a)
        a.label(r.label('prolog end'))
        setup(a, r, log)
        body(a, r)
        a.li(3, 0)
        a.b(r.label('epilog'))
        targets = {target for _, _, _, target in scopes if target}
        for target in sorted(targets):
            a.label(r.label(target))
            c_resumed(a, r, log, int(target.split()[-1]))
        a.label(r.label('recovery'))
        c_resumed(a, r, log, 0)
        a.label(r.label('epilog'))
        epilog(a)
        a.label(r.label('end'))
        r.function(r.entry, r.label('prolog end'), r.label('end'),
                   handler=True)
        if extra:
            extra(a, r, thunk, log)
        for index, (result, fix) in enumerate(filters):
            c_filter(a, r, log, index, result, fix)
        scope_table(a, r.label('scopes'), [
            (r.label(begin), r.label(end), r.label(handler),
             r.label(target) if target else None)
            for begin, end, handler, target in scopes])

    routine = s.routine(f'{name} probe', emit)
    s.call(routine, NULL, name=name, capture=c_captures(log))


def c_load(a, r):
    a.label(r.label('try'))
    a.lwz(4, 0, 3)
    a.stw(4, C_LOADED, 11)
    a.label(r.label('try end'))


def c_nested_load(a, r):
    a.label(r.label('outer try'))
    a.nop()
    a.label(r.label('try'))
    a.lwz(4, 0, 3)
    a.stw(4, C_LOADED, 11)
    a.label(r.label('try end'))
    a.nop()
    a.label(r.label('outer try end'))


def c_call(a, r):
    a.label(r.label('try'))
    a.nop()
    a.label(r.label('call'))
    a.bl(r.label('leaf'))
    a.label(r.label('after call'))
    a.nop()
    a.label(r.label('try end'))


def c_leaf(a, r, thunk, log):
    """A frameless leaf making the access, with the log in r11."""
    a.label(r.label('leaf'))
    a.lwz(4, 0, 3)
    a.stw(4, C_LOADED, 11)
    a.blr()
    a.label(r.label('leaf end'))
    r.function(r.label('leaf'), r.label('leaf'), r.label('leaf end'))


def c_inner(a, r, thunk, log, finally_handler=None):
    """A framed function with a finally handler around a call to the leaf,
    which logs its entry sp and sets r31 to INNER_MARKER. The finally handler
    logs its call, or is what `finally_handler(a, r, thunk, log)` emits at the
    label 'finally'."""
    c_words(a, thunk, r.label('inner scopes'))
    a.label(r.label('inner'))
    save_frame(a)
    a.label(r.label('inner prolog end'))
    a.load32(11, log.address)
    a.addi(12, 1, FRAME)
    a.stw(12, C_INNER_ENTRY_SP, 11)
    a.load32(31, INNER_MARKER)
    a.sync()
    a.label(r.label('inner try'))
    a.bl(r.label('leaf'))
    a.label(r.label('inner try end'))
    a.li(3, 0)
    epilog(a)
    a.label(r.label('inner end'))
    r.function(r.label('inner'), r.label('inner prolog end'),
               r.label('inner end'), handler=True)
    c_leaf(a, r, thunk, log)
    if finally_handler:
        finally_handler(a, r, thunk, log)
    else:
        a.label(r.label('finally'))
        c_finally_logged(a, log)
        a.blr()
    scope_table(a, r.label('inner scopes'), [
        (r.label('inner try'), r.label('inner try end'), r.label('finally'),
         None)])


def c_finally_logged(a, log):
    """Logs a finally handler's call, made with r3 1 for an abnormal
    termination and r12 the establisher."""
    a.load32(11, log.address)
    a.lwz(10, C_FINALLY, 11)
    a.addi(0, 10, 1)
    a.stw(0, C_FINALLY, 11)
    a.stw(3, C_FINALLY + 4, 11)
    a.lwz(10, C_INNER_ENTRY_SP, 11)
    a.subf(0, 10, 12)
    a.stw(0, C_FINALLY + 8, 11)


def raising_finally(record):
    """A framed finally handler that logs its call and raises `record`."""
    def emit(a, r, thunk, log):
        a.label(r.label('finally'))
        a.mflr(0)
        a.stw(0, -8, 1)
        a.stwu(1, -HANDLER_FRAME, 1)
        a.label(r.label('finally prolog end'))
        c_finally_logged(a, log)
        a.load32(3, record.address)
        a.call(thunk('RtlRaiseException'))
        a.addi(1, 1, HANDLER_FRAME)
        a.lwz(0, -8, 1)
        a.mtlr(0)
        a.blr()
        a.label(r.label('finally end'))
        r.function(r.label('finally'), r.label('finally prolog end'),
                   r.label('finally end'))
    return emit


def c_call_inner(a, r):
    a.label(r.label('try'))
    a.bl(r.label('inner'))
    a.label(r.label('try end'))


RECORD_SIZE = 0x50
RAISE_CODE = 0xE0000001
NESTED_RAISE_CODE = 0xE0000002
RAISE_PARAMETERS = (0x11111111, 0x22222222)
EXCEPTION_NONCONTINUABLE = 0x1
# What no handler may return, searching or unwinding.
INVALID_DISPOSITION = 4
# How a raise handler logs a record pointer.
NO_RECORD = 0
OTHER_RECORD = 3


def exception_record(s, name, code, flags=0, parameters=()):
    data = struct.pack('>5I', code, flags, 0, 0, len(parameters))
    data += struct.pack(f'>{len(parameters)}I', *parameters)
    return s.alloc(RECORD_SIZE, name, data, fill=0)


def record_captures(records):
    """The records but for their addresses."""
    captures = []
    for record in records:
        captures += [record.prefix(RECORD_ADDRESS),
                     (record + RECORD_PARAMETERS).prefix(0xC)]
    return captures


def raise_handler(s, log, actions, records, first_logged=0):
    """A framed language handler that logs its first LOGGED_CALLS calls into a
    scripted handler's log and acts on call k as actions[k] says, searching
    on past the end. Per call it logs the record's code and flags, which
    record it chains to and which it is, its parameter count and first two
    parameters (0 past the count), the establisher frame against the probe's
    entry sp, the dispatcher context's control pc and function entry, its
    establisher frame and context record against the arguments, and the
    CONTEXT's r31 for the corpus's own records. Records are told by their
    number in `records` from 1, NO_RECORD or OTHER_RECORD. The actions are
    'continue' at the probe's recovery label with r3 0, 'return' to where it
    was raised, 'search', 'unwind' to the frame whose handler it is at its
    probe's recovery label with UNWIND_VALUE, 'raise' the second record with
    r31 FAULT_R31, and 'invalid' to return INVALID_DISPOSITION. It logs the
    calls from `first_logged` on."""
    assert len(records) < OTHER_RECORD

    def emit(a, r, thunk):
        a.label(r.entry)
        a.mflr(12)
        a.stw(12, -8, 1)
        a.std(31, -0x10, 1)
        a.stwu(1, -HANDLER_FRAME, 1)
        a.label(r.label('prolog end'))
        a.load32(11, log.address)
        a.lwz(12, H_CALLS, 11)
        a.addi(0, 12, 1)
        a.stw(0, H_CALLS, 11)
        if first_logged:
            a.addi(10, 12, -first_logged)
            a.cmplwi(10, LOGGED_CALLS)
            a.bge(r.label('act'))
            a.mulli(10, 10, CALL_RECORD)
        else:
            a.cmpwi(12, LOGGED_CALLS)
            a.bge(r.label('act'))
            a.mulli(10, 12, CALL_RECORD)
        a.add(10, 10, 11)
        out = H_RECORDS

        def store(reg):
            nonlocal out
            a.stw(reg, out, 10)
            out += 4

        def copy(base, offset):
            a.lwz(0, offset, base)
            store(0)

        def difference(minuend_reg, subtrahend_reg):
            a.subf(0, subtrahend_reg, minuend_reg)
            store(0)

        def record_number(reg, what):
            """Logs which record reg points at, leaving it in r0."""
            done = r.label(f'{what} told')
            a.li(0, NO_RECORD)
            a.cmpwi(reg, 0)
            a.beq(done)
            for number, record in enumerate(records, 1):
                a.load32(9, record.address)
                a.li(0, number)
                a.cmpw(reg, 9)
                a.beq(done)
            a.li(0, OTHER_RECORD)
            a.label(done)
            store(0)

        copy(3, RECORD_CODE)
        copy(3, RECORD_FLAGS)
        a.lwz(8, RECORD_CHAIN, 3)
        record_number(8, 'chained record')
        record_number(3, 'record')
        a.mr(7, 0)
        copy(3, RECORD_PARAMETERS)
        for n in range(2):
            a.lwz(9, RECORD_PARAMETERS, 3)
            a.li(0, 0)
            a.cmpwi(9, n)
            a.ble(r.label(f'parameter {n} logged'))
            a.lwz(0, RECORD_INFORMATION + 4 * n, 3)
            a.label(r.label(f'parameter {n} logged'))
            store(0)
        a.lwz(9, L_ENTRY_SP, 11)
        difference(4, 9)
        copy(6, 0x0)
        copy(6, 0x4)
        a.lwz(9, DISPATCHER_ESTABLISHER, 6)
        difference(9, 4)
        a.lwz(9, DISPATCHER_CONTEXT, 6)
        difference(9, 5)
        a.li(0, 0)
        a.cmpwi(7, OTHER_RECORD)
        a.beq(r.label('r31 logged'))
        a.lwz(0, gpr_low(31), 5)
        a.label(r.label('r31 logged'))
        store(0)
        assert out == H_RECORDS + CALL_RECORD
        a.sync()

        a.label(r.label('act'))
        for k in range(len(actions)):
            a.cmpwi(12, k)
            a.beq(r.label(f'action {k}'))
        a.b(r.label('search'))
        for k, action in enumerate(actions):
            a.label(r.label(f'action {k}'))
            if action == 'continue':
                a.lwz(0, L_RECOVERY, 11)
                a.stw(0, CONTEXT_IAR, 5)
                a.li(0, 0)
                a.stw(0, CONTEXT_GPR + 8, 5)
                a.stw(0, CONTEXT_GPR + 8 * 3, 5)
                a.stw(0, gpr_low(3), 5)
                a.lwz(0, L_BODY_SP, 11)
                a.stw(0, gpr_low(1), 5)
                a.li(3, 0)              # ExceptionContinueExecution
                a.b(r.label('return'))
            elif action == 'return':
                a.li(3, 0)              # ExceptionContinueExecution
                a.b(r.label('return'))
            elif action == 'invalid':
                a.li(3, INVALID_DISPOSITION)
                a.b(r.label('return'))
            elif action == 'search':
                a.b(r.label('search'))
            elif action == 'unwind':
                a.mr(5, 3)
                a.mr(3, 4)
                a.lwz(4, L_RECOVERY, 11)
                a.load32(6, UNWIND_VALUE)
                a.call(thunk('RtlUnwind'))
                a.b(r.label('search'))
            elif action == 'raise':
                a.li(31, FAULT_R31)
                a.load32(3, records[1].address)
                a.call(thunk('RtlRaiseException'))
                a.b(r.label('search'))
            else:
                raise ValueError(f'unknown action {action}')
        a.label(r.label('search'))
        a.li(3, 1)                      # ExceptionContinueSearch
        a.label(r.label('return'))
        a.addi(1, 1, HANDLER_FRAME)
        a.lwz(12, -8, 1)
        a.mtlr(12)
        a.ld(31, -0x10, 1)
        a.blr()
        a.label(r.label('end'))
        r.function(r.entry, r.label('prolog end'), r.label('end'))
    return s.routine('raise handler', emit)


def raises(record, log):
    """A body raising `record`, which logs r1 against the probe's sp and r31
    if the raise returns."""
    def body(a, r, thunk):
        a.load32(3, record.address)
        a.call(thunk('RtlRaiseException'))
        a.load32(11, log.address)
        a.lwz(9, L_BODY_SP, 11)
        a.subf(0, 9, 1)
        a.stw(0, H_RESUMED + 4, 11)
        a.stw(31, H_RESUMED + 8, 11)
    return body


def raise_scenario(corpus, name, actions, flags=0, nested=False):
    """A probe raising a record with `flags`, with a raise handler. A
    `nested` one has a second record for the handler to raise."""
    s = corpus.scenario(name)
    log = s.alloc(H_SIZE, 'log', fill=0)
    records = [exception_record(s, 'record', RAISE_CODE, flags,
                                RAISE_PARAMETERS)]
    if nested:
        records.append(exception_record(s, 'nested record',
                                        NESTED_RAISE_CODE))
    handler = raise_handler(s, log, actions, records)
    routine = resumable(s, f'{name} probe', log, handler,
                        raises(records[0], log))
    s.call(routine, NULL, name=name,
           capture=h_captures(log) + record_captures(records))


def raise_outer_scenario(corpus, name, outer_actions, inner_actions):
    """A probe with a raise handler calling another, which raises, each with
    a log and marker of its own."""
    s = corpus.scenario(name)
    outer_log = s.alloc(H_SIZE, 'outer log', fill=0)
    inner_log = s.alloc(H_SIZE, 'inner log', fill=0)
    record = exception_record(s, 'record', RAISE_CODE, 0, RAISE_PARAMETERS)
    inner = resumable(s, f'{name} inner', inner_log,
                      raise_handler(s, inner_log, inner_actions, [record]),
                      raises(record, inner_log), marker=INNER_MARKER)
    outer = resumable(s, f'{name} outer', outer_log,
                      raise_handler(s, outer_log, outer_actions, [record]),
                      calls(inner))
    s.call(outer, NULL, name=name,
           capture=h_captures(outer_log) + h_captures(inner_log) +
           record_captures([record]))


def collided_scenario(corpus, name):
    """A probe with a raise handler that unwinds, calling one whose raise
    handler raises a second record as that unwind passes it. The outer handler
    unwinds that one too, colliding with its first unwind. The inner handler's
    log starts at its second call."""
    s = corpus.scenario(name)
    outer_log = s.alloc(H_SIZE, 'outer log', fill=0)
    inner_log = s.alloc(H_SIZE, 'inner log', fill=0)
    records = [exception_record(s, 'record', RAISE_CODE, 0, RAISE_PARAMETERS),
               exception_record(s, 'nested record', NESTED_RAISE_CODE)]
    inner_handler = raise_handler(s, inner_log,
                                  ['search', 'raise', 'search', 'search'],
                                  records, first_logged=1)
    inner = resumable(s, f'{name} inner', inner_log, inner_handler,
                      raises(records[0], inner_log), marker=INNER_MARKER)
    outer = resumable(s, f'{name} outer', outer_log,
                      raise_handler(s, outer_log,
                                    ['unwind', 'unwind', 'search'], records),
                      calls(inner))
    s.call(outer, NULL, name=name,
           capture=h_captures(outer_log) + h_captures(inner_log) +
           record_captures(records))


def collided_finally_scenario(corpus, name):
    """A __C_specific_handler probe whose filter takes every exception,
    calling a function whose finally handler raises a record as the unwind of
    the fault in its leaf passes it. The filter takes that one too, and its
    unwind collides with the first."""
    s = corpus.scenario(name)
    log = s.alloc(C_SIZE, 'log', fill=0)
    record = exception_record(s, 'record', NESTED_RAISE_CODE)

    def emit(a, r, thunk):
        c_words(a, thunk, r.label('scopes'))
        a.label(r.entry)
        save_frame(a)
        a.label(r.label('prolog end'))
        setup(a, r, log)
        c_call_inner(a, r)
        a.li(3, 0)
        a.b(r.label('epilog'))
        a.label(r.label('except 1'))
        c_resumed(a, r, log, 1)
        a.label(r.label('recovery'))
        c_resumed(a, r, log, 0)
        a.label(r.label('epilog'))
        epilog(a)
        a.label(r.label('end'))
        r.function(r.entry, r.label('prolog end'), r.label('end'),
                   handler=True)
        c_inner(a, r, thunk, log, raising_finally(record))
        c_filter(a, r, log, 0, 1)
        scope_table(a, r.label('scopes'), [
            (r.label('try'), r.label('try end'), r.label('filter 0'),
             r.label('except 1'))])

    routine = s.routine(f'{name} probe', emit)
    s.call(routine, NULL, name=name,
           capture=c_captures(log) + record_captures([record]))


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

    # A fault in a handler is nested in the frames up to the one that handler
    # ran for. Its own frame's handler, called again, can continue it.
    handler_scenario(corpus, 'nested fault', ['fault', 'continue'])
    # The frame the handler ran for is the last the nested fault is flagged
    # in, so the one past it sees an ordinary fault.
    outer_scenario(corpus, 'nested fault searched on', ['continue'],
                   ['fault', 'search'])

    # RtlUnwind2 with no exception record, from code that didn't fault.
    unwind_without_exception(corpus)
    # A handler unwinding to its own frame, which is called again to unwind.
    handler_scenario(corpus, 'unwind', ['unwind', 'search'])
    # The frames on the way are unwound too, restoring what they saved.
    outer_scenario(corpus, 'unwind through a frame', ['unwind', 'search'],
                   ['search', 'search'])
    handler_scenario(corpus, 'unwind2', ['unwind2', 'search'],
                     with_context=True)

    # __C_specific_handler runs the filters of the scopes the control pc is
    # in and unwinds to the except label of the first that returns 1.
    one_scope = [('try', 'try end', 'filter 0', 'except 1')]
    c_scenario(corpus, 'except', c_load, one_scope, [(1, False)])
    c_scenario(corpus, 'except continue', c_load, one_scope, [(-1, True)])
    c_scenario(corpus, 'except search', c_nested_load,
               [('try', 'try end', 'filter 0', 'except 1'),
                ('outer try', 'outer try end', 'filter 1', 'except 2')],
               [(0, False), (1, False)])
    # A caller's control pc is its call.
    c_scenario(corpus, 'except at call', c_call,
               [('try', 'call', 'filter 0', 'except 1'),
                ('call', 'after call', 'filter 1', 'except 2'),
                ('after call', 'try end', 'filter 2', 'except 3')],
               [(1, False)] * 3, extra=c_leaf)
    # A finally handler on the way runs as the unwind passes it.
    c_scenario(corpus, 'finally', c_call_inner, one_scope, [(1, False)],
               extra=c_inner)

    # A frameless leaf whose prolog saved r31 below its stack pointer faults
    # to its caller's handler.
    leaf_scenario(corpus, 'leaf save fault', leaf_saving_in_prolog)
    # The handler continues a frameless leaf, whose saves below its stack
    # pointer stay as they were.
    leaf_scenario(corpus, 'leaf red zone', leaf_saving_in_body)
    # A fault in a prolog after its stwu, which unwinding the frame undoes.
    leaf_scenario(corpus, 'prolog fault', faulting_prolog)
    # An unwind from the handler of a call to no code goes on from the call
    # to the probe's frame.
    handler_scenario(corpus, 'unwind call fault', ['unwind', 'search'],
                     body=lambda a, r, thunk: call_target(a))

    # A raise goes to the handlers from its caller's frame, with the caller's
    # own record. One continues it at the recovery label.
    raise_scenario(corpus, 'raise continue', ['continue'])
    # Continuing it unchanged returns from the raise.
    raise_scenario(corpus, 'raise return', ['return'])
    # A handler unwinding to its own frame, which is called again to unwind.
    raise_scenario(corpus, 'raise unwind', ['unwind', 'search'])
    # A frame searching on passes it to its caller, whose control pc is its
    # call, and the unwind from there passes the raising frame.
    raise_outer_scenario(corpus, 'raise searched on', ['unwind', 'search'],
                         ['search', 'search'])
    # A raise in a handler is nested in the frames up to the one that handler
    # ran for.
    raise_scenario(corpus, 'raise in a handler', ['raise', 'continue'],
                   nested=True)
    # Continuing a noncontinuable exception raises another, which this
    # handler unwinds.
    raise_scenario(corpus, 'noncontinuable continued',
                   ['continue', 'unwind', 'search'],
                   flags=EXCEPTION_NONCONTINUABLE)
    # A handler returning what none may raises another, which this handler
    # unwinds.
    raise_scenario(corpus, 'invalid disposition',
                   ['invalid', 'unwind', 'search'])
    # Continuing that one raises a third, chained to it.
    raise_scenario(corpus, 'invalid disposition continued',
                   ['invalid', 'continue', 'unwind', 'search'])
    # One returned while unwinding is raised from the call that unwinds,
    # here in the handler, so it is nested.
    raise_scenario(corpus, 'invalid disposition in an unwind',
                   ['unwind', 'invalid', 'unwind', 'search'])
    # An unwind reaching another's handler call takes that one over from the
    # frame whose handler it called, which is called again.
    collided_scenario(corpus, 'collided unwind')
    # The same through a finally handler, whose scope the collided call skips.
    collided_finally_scenario(corpus, 'collided unwind through a finally')
