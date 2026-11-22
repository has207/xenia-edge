"""Unwind corpus: what XexPcToFileHeader reports for addresses inside and
outside the executor's image, and what RtlCaptureContext writes, the two calls
an unwinder starts from.

The executor image is loaded at 0x82000000 with its code from 0x82010000.
Buffers start out as 0xCD, so bytes a call leaves alone stay 0xCD.

Left out because Xenia differs by design: the console's XexPcToFileHeader
also finds xam and the kernel (0x815F0000 and 0x80040000 for 17559), which
have no guest image in Xenia, and the loader entries it stores are different
addresses. RtlCaptureContext's iar is the instruction after the kernel's sc
(Xenia reports the caller's import thunk), ctr the kernel function the thunk
branched to, r0, r1, r11-r13 come from the thunk and the kernel, cr and xer
have bits set that the executor never sets (cr1, cr6, cr7 and CA), and the
console stores a stack address at 0x228 where Xenia stores 0.
"""

EXECUTOR_BASE = 0x82000000
EXECUTOR_CODE = 0x82010000
NO_IMAGE_ADDRESS = 0x40000000

CONTEXT_SIZE = 0xA40
GUARD_SIZE = 0x40


def gpr(context, first, count=1):
    return (context + 0x18 + first * 8).prefix(count * 8)


def capture_lr_scenario(corpus):
    """RtlCaptureContext called three ways, a bl to the import thunk, a bctrl
    right after the mtctr and one two instructions after it. Each logs the
    lr it captured against the address its call returns to."""
    s = corpus.scenario('capture context lr')
    log = s.alloc(0xC, 'lr log', fill=0)
    context = s.alloc(CONTEXT_SIZE, 'lr context', fill=0)

    def emit(a, r, thunk):
        target = thunk('RtlCaptureContext')

        def logged(k):
            a.label(r.label(f'returned {k}'))
            a.load32(11, context.address)
            a.lwz(0, 0xC, 11)
            a.load_address(9, r.label(f'returned {k}'))
            a.subf(0, 9, 0)
            a.load32(11, log.address)
            a.stw(0, 4 * k, 11)

        a.label(r.entry)
        a.mflr(0)
        a.stw(0, -8, 1)
        a.stwu(1, -0x60, 1)
        a.label(r.label('prolog end'))
        a.load32(3, context.address)
        a.word(0x48000001 | ((target - a.address()) & 0x03FFFFFC))
        logged(0)
        a.load32(3, context.address)
        a.call(target)
        logged(1)
        a.load32(3, context.address)
        a.load32(12, target)
        a.mtctr(12)
        a.nop()
        a.nop()
        a.bctrl()
        logged(2)
        a.addi(1, 1, 0x60)
        a.lwz(0, -8, 1)
        a.mtlr(0)
        a.li(3, 0)
        a.blr()
        a.label(r.label('end'))
        r.function(r.entry, r.label('prolog end'), r.label('end'))

    routine = s.routine('capture context lr probe', emit)
    s.call(routine, name='capture context lr', capture=[log])


def build(corpus):
    s = corpus.scenario('pc to file header')
    for name, pc in (('executor code', EXECUTOR_CODE),
                     ('executor base', EXECUTOR_BASE)):
        ldr = s.alloc(8, 'ldr')
        s.call('XexPcToFileHeader', pc, ldr, name=name)
    # A miss leaves the loader entry pointer alone.
    for name, pc in (('no image', NO_IMAGE_ADDRESS), ('null', 0)):
        ldr = s.alloc(8, 'ldr')
        s.call('XexPcToFileHeader', pc, ldr, capture=[ldr.prefix(4)],
               name=name)

    s = corpus.scenario('capture context')
    context = s.alloc(CONTEXT_SIZE + GUARD_SIZE, 'context')
    s.call('RtlCaptureContext', context, void=True, capture=[
        context.prefix(0x8),              # flags, msr
        (context + 0xC).prefix(0x4),      # lr
        gpr(context, 3),
        gpr(context, 14, 18),
        (context + 0x120).prefix(0x108),  # fpscr, fpr
        (context + 0x22C).prefix(0x4),    # not written
        (context + 0x230).prefix(0x810),  # vscr, vr
        context + CONTEXT_SIZE,           # guard
    ])

    capture_lr_scenario(corpus)
