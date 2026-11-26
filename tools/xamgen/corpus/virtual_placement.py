"""Virtual placement corpus: where NtAllocateVirtualMemory puts a region it
picks the base of, relative to the region before it, for 4 KB and 64 KB pages,
bottom-up and top-down.

A region's base depends on the title's layout, so a routine reduces each base
to its distance from the one before, and to its low 16 bits for the 64 KB
boundary every region starts on. The scenarios reserve two one-page regions,
a region past 64 KB after one page, and a region again after releasing the one
it replaces. Each releases its regions.
"""

import struct

MEM_RESERVE = 0x2000
MEM_RELEASE = 0x8000
MEM_TOP_DOWN = 0x100000
MEM_LARGE_PAGES = 0x20000000
PAGE_READWRITE = 4

PAGE_SIZES = (('4 KB', 0x1000, 0), ('64 KB', 0x10000, MEM_LARGE_PAGES))
DIRECTIONS = (('bottom-up', 0), ('top-down', MEM_TOP_DOWN))


class Routines:
    def __init__(self, s):
        self.s = s
        self.made = {}

    def get(self, name, emit):
        if name not in self.made:
            self.made[name] = self.s.routine(name, emit)
        return self.made[name]

    def difference(self):
        """r3 less r4."""
        def emit(a, r, thunk):
            a.label(r.entry)
            a.subf(3, 4, 3)
            a.blr()
        return self.get('difference', emit)

    def low16(self):
        """The low 16 bits of r3."""
        def emit(a, r, thunk):
            a.label(r.entry)
            a.rlwinm(3, 3, 0, 16, 31)
            a.blr()
        return self.get('low 16 bits', emit)


class Regions:
    """Reserves and releases a scenario's regions through one base and size."""

    def __init__(self, s, flags):
        self.s = s
        self.flags = flags
        self.base = s.alloc(4, 'base', fill=0)
        self.size = s.alloc(4, 'size', fill=0)

    def reserve(self, name, length):
        region = self.s.variable(name)
        self.s.write(self.base, struct.pack('>I', 0))
        self.s.write(self.size, struct.pack('>I', length))
        self.s.call('NtAllocateVirtualMemory', self.base, self.size,
                    MEM_RESERVE | self.flags, PAGE_READWRITE, 0,
                    name=f'reserve {name}', capture=[self.size],
                    store=[(region, self.base)])
        return region

    def release(self, region, name):
        self.s.call('RtlFillMemoryUlong', self.base, 4, region,
                    name=f'point at {name}', void=True)
        self.s.write(self.size, struct.pack('>I', 0))
        self.s.call('NtFreeVirtualMemory', self.base, self.size, MEM_RELEASE,
                    0, name=f'release {name}')


def build(corpus):
    for size_name, page, large in PAGE_SIZES:
        # Past 64 KB, by a page for 4 KB pages and two for 64 KB pages.
        past = 0x11000 if page == 0x1000 else 0x30000
        for direction, top_down in DIRECTIONS:
            suffix = f'{size_name} pages, {direction}'

            s = corpus.scenario(f'two one-page regions, {suffix}')
            routines = Routines(s)
            regions = Regions(s, large | top_down)
            first = regions.reserve('first', page)
            second = regions.reserve('second', page)
            s.call(routines.difference(), second, first,
                   name='second less first')
            s.call(routines.low16(), first, name='low 16 bits of first')
            s.call(routines.low16(), second, name='low 16 bits of second')
            regions.release(first, 'first')
            regions.release(second, 'second')

            s = corpus.scenario(f'a region past 64 KB after one page, '
                                f'{suffix}')
            routines = Routines(s)
            regions = Regions(s, large | top_down)
            first = regions.reserve('first', page)
            second = regions.reserve('second', past)
            s.call(routines.difference(), second, first,
                   name='second less first')
            regions.release(first, 'first')
            regions.release(second, 'second')

            s = corpus.scenario(f'a region again after releasing it, '
                                f'{suffix}')
            routines = Routines(s)
            regions = Regions(s, large | top_down)
            first = regions.reserve('first', page)
            second = regions.reserve('second', page)
            regions.release(second, 'second')
            third = regions.reserve('third', page)
            s.call(routines.difference(), third, second,
                   name='third less second')
            regions.release(first, 'first')
            regions.release(third, 'third')
