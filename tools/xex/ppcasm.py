#!/usr/bin/env python3
"""A tiny big-endian PPC assembler for test titles.

Instructions are methods named after their mnemonics. Branches take label
names that place() binds, forward or backward, and code() resolves them, as
it does a label's absolute address in a data word or a lis/ori pair.

    asm = Assembler(0x82010000)
    asm.li(3, 0)
    asm.label('loop')
    asm.addic_(3, 3, -1)
    asm.bne('loop')
    text = asm.code()
"""

import struct


SIGNED = (-0x8000, 0x7FFF)
UNSIGNED = (0, 0xFFFF)
# addis takes the high half of a value either way.
HIGH_HALF = (-0x8000, 0xFFFF)


def _d(value, bounds=SIGNED):
    if not bounds[0] <= value <= bounds[1]:
        raise ValueError(f'immediate {value:#x} out of range')
    return value & 0xFFFF


class Assembler:
    def __init__(self, base):
        self.base = base
        self.words = []
        self.labels = {}
        self.fixups = []  # (index, label, kind)

    def address(self):
        return self.base + 4 * len(self.words)

    def label(self, name):
        if name in self.labels:
            raise ValueError(f'label {name} placed twice')
        self.labels[name] = len(self.words)

    def word(self, value):
        self.words.append(value & 0xFFFFFFFF)

    def label_address(self, name):
        if name not in self.labels:
            raise ValueError(f'label {name} never placed')
        return self.base + 4 * self.labels[name]

    def code(self):
        for index, name, kind in self.fixups:
            if name not in self.labels:
                raise ValueError(f'label {name} never placed')
            offset = 4 * (self.labels[name] - index)
            if kind == 'abs':
                self.words[index] = self.label_address(name)
            elif kind in ('hi', 'lo'):
                address = self.label_address(name)
                self.words[index] |= (address >> 16 if kind == 'hi'
                                      else address & 0xFFFF)
            elif kind == 'b':
                if not -0x2000000 <= offset < 0x2000000:
                    raise ValueError('branch out of range')
                self.words[index] |= offset & 0x3FFFFFC
            else:
                if not -0x8000 <= offset < 0x8000:
                    raise ValueError('conditional branch out of range')
                self.words[index] |= offset & 0xFFFC
        return b''.join(struct.pack('>I', w) for w in self.words)

    # D-form.
    def _dform(self, op, rt, ra, imm, bounds=SIGNED):
        self.word((op << 26) | (rt << 21) | (ra << 16) | _d(imm, bounds))

    def addi(self, rt, ra, imm): self._dform(14, rt, ra, imm)
    def addis(self, rt, ra, imm): self._dform(15, rt, ra, imm, HIGH_HALF)
    def addic_(self, rt, ra, imm): self._dform(13, rt, ra, imm)
    def mulli(self, rt, ra, imm): self._dform(7, rt, ra, imm)
    def li(self, rt, imm): self.addi(rt, 0, imm)
    def lis(self, rt, imm): self.addis(rt, 0, imm)
    def ori(self, ra, rs, imm): self._dform(24, rs, ra, imm, UNSIGNED)
    def andi_(self, ra, rs, imm): self._dform(28, rs, ra, imm, UNSIGNED)
    def lwz(self, rt, d, ra): self._dform(32, rt, ra, d)
    def lbz(self, rt, d, ra): self._dform(34, rt, ra, d)
    def stw(self, rs, d, ra): self._dform(36, rs, ra, d)
    def stwu(self, rs, d, ra): self._dform(37, rs, ra, d)
    def stb(self, rs, d, ra): self._dform(38, rs, ra, d)

    def cmpwi(self, ra, imm): self._dform(11, 0, ra, imm)
    def cmplwi(self, ra, imm): self._dform(10, 0, ra, imm, UNSIGNED)

    # DS-form, offsets a multiple of 4.
    def _dsform(self, op, rt, ra, d, xo):
        if d % 4:
            raise ValueError('DS offset not a multiple of 4')
        self.word((op << 26) | (rt << 21) | (ra << 16) | (_d(d) & 0xFFFC) | xo)

    def ld(self, rt, d, ra): self._dsform(58, rt, ra, d, 0)
    def std(self, rs, d, ra): self._dsform(62, rs, ra, d, 0)

    # X- and XO-form.
    def _xform(self, rt, ra, rb, xo, rc=0):
        self.word((31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) |
                  (xo << 1) | rc)

    def add(self, rt, ra, rb): self._xform(rt, ra, rb, 266)
    def subf(self, rt, ra, rb): self._xform(rt, ra, rb, 40)
    def or_(self, ra, rs, rb): self._xform(rs, ra, rb, 444)
    def mr(self, ra, rs): self.or_(ra, rs, rs)
    def cmpw(self, ra, rb): self._xform(0, ra, rb, 0)
    def cmplw(self, ra, rb): self._xform(0, ra, rb, 32)
    def lwzx(self, rt, ra, rb): self._xform(rt, ra, rb, 23)
    def sync(self): self.word(0x7C0004AC)

    def rlwinm(self, ra, rs, sh, mb, me):
        self.word((21 << 26) | (rs << 21) | (ra << 16) | (sh << 11) |
                  (mb << 6) | (me << 1))

    def slwi(self, ra, rs, n): self.rlwinm(ra, rs, n, 0, 31 - n)

    def clrrwi(self, ra, rs, n): self.rlwinm(ra, rs, 0, 0, 31 - n)

    def mftb(self, rt): self.word(0x7C0C42E6 | (rt << 21))
    def mtctr(self, rs): self.word(0x7C0903A6 | (rs << 21))
    def mflr(self, rt): self.word(0x7C0802A6 | (rt << 21))
    def mtlr(self, rs): self.word(0x7C0803A6 | (rs << 21))
    def bctr(self): self.word(0x4E800420)
    def bctrl(self): self.word(0x4E800421)
    def blr(self): self.word(0x4E800020)
    def nop(self): self.word(0x60000000)

    # Branches to labels.
    def b(self, label):
        self.fixups.append((len(self.words), label, 'b'))
        self.word(0x48000000)

    def bl(self, label):
        self.fixups.append((len(self.words), label, 'b'))
        self.word(0x48000001)

    def _bc(self, bo, bi, label):
        self.fixups.append((len(self.words), label, 'bc'))
        self.word((16 << 26) | (bo << 21) | (bi << 16))

    def beq(self, label): self._bc(12, 2, label)
    def bne(self, label): self._bc(4, 2, label)
    def blt(self, label): self._bc(12, 0, label)
    def bge(self, label): self._bc(4, 0, label)
    def bgt(self, label): self._bc(12, 1, label)
    def ble(self, label): self._bc(4, 1, label)

    # Composites.
    def load32(self, rt, value):
        self.lis(rt, (value >> 16) & 0xFFFF)
        self.ori(rt, rt, value & 0xFFFF)

    def call(self, target):
        """Calls `target` through ctr with r12."""
        self.load32(12, target)
        self.mtctr(12)
        self.bctrl()

    def load_address(self, rt, label):
        """Loads the absolute address of `label`."""
        self.fixups.append((len(self.words), label, 'hi'))
        self.lis(rt, 0)
        self.fixups.append((len(self.words), label, 'lo'))
        self.ori(rt, rt, 0)

    def address_word(self, label):
        """A data word holding the absolute address of `label`."""
        self.fixups.append((len(self.words), label, 'abs'))
        self.word(0)
