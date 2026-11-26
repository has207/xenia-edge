"""Memory statistics corpus: the title's and the system's available pages that
MmQueryStatistics reports, against the physical memory each can allocate.

The counts depend on everything else in memory, so a scenario records only
whether the title's count fits in the title's partition and how both counts
change when it allocates a block of physical memory and frees it again. The
title's blocks are in a window from 256 MB up, the system's anywhere in its
partition.
"""

import struct

PAGE_READWRITE = 0x04
MEM_LARGE_PAGES = 0x20000000
MEM_16MB_PAGES = 0x80000000

STATISTICS_SIZE = 0x68
TITLE_AVAILABLE = 0x0C
SYSTEM_AVAILABLE = 0x38
TITLE_PARTITION_PAGES = 0x1E000
SYSTEM_PARTITION_END = 0x02000000

BLOCKS = (('4 KB', 0x1000, PAGE_READWRITE, 0x10000000),
          ('64 KB', 0x10000, MEM_LARGE_PAGES | PAGE_READWRITE, 0x10100000),
          ('16 MB', 0x1000000, MEM_16MB_PAGES | PAGE_READWRITE, 0x18000000))


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

    def at_most(self):
        """1 if r3 is at most r4, unsigned, else 0."""
        def emit(a, r, thunk):
            a.label(r.entry)
            a.cmplw(3, 4)
            a.li(3, 1)
            a.ble(r.label('done'))
            a.li(3, 0)
            a.label(r.label('done'))
            a.blr()
        return self.get('at most', emit)

    def free(self):
        """MmFreePhysicalMemory(r3, r4) if the block at r5 was allocated."""
        def emit(a, r, thunk):
            a.label(r.entry)
            a.cmplwi(5, 0)
            a.beq(r.label('none'))
            a.load32(12, thunk('MmFreePhysicalMemory'))
            a.mtctr(12)
            a.bctr()
            a.label(r.label('none'))
            a.blr()
        return self.get('free if allocated', emit)


def query(s, when):
    """The title's and the system's available pages, `when` naming the
    moment."""
    stats = s.alloc(STATISTICS_SIZE, f'statistics {when}',
                    struct.pack('>I', STATISTICS_SIZE), fill=0)
    title = s.variable(f'title available {when}')
    system = s.variable(f'system available {when}')
    s.call('MmQueryStatistics', stats, name=f'statistics {when}',
           store=[(title, (stats + TITLE_AVAILABLE).prefix(4)),
                  (system, (stats + SYSTEM_AVAILABLE).prefix(4))])
    return title, system


def changes(s, routines, before, after, what):
    for which, earlier, later in (('title', before[0], after[0]),
                                  ('system', before[1], after[1])):
        s.call(routines.difference(), earlier, later,
               name=f'{which} pages {what}')


def block_scenario(corpus, which, size_name, size, protect, type_, low,
                   high):
    s = corpus.scenario(f'a block of {which} memory, {size_name} pages')
    routines = Routines(s)
    before = query(s, 'before')
    block = s.variable('block')
    s.call('MmAllocatePhysicalMemoryEx', type_, size, protect, low, high, 0,
           name='allocate block', store=[(block, 'r3')], void=True)
    s.call(routines.at_most(), 1, block, name='block allocated')
    allocated = query(s, 'allocated')
    s.call(routines.free(), type_, block, block, name='free block', void=True)
    freed = query(s, 'freed')
    changes(s, routines, before, allocated, 'the block took')
    changes(s, routines, freed, allocated, 'its free gave back')


def build(corpus):
    s = corpus.scenario('the title\'s available pages in its partition')
    routines = Routines(s)
    title, _ = query(s, 'now')
    s.call(routines.at_most(), title, TITLE_PARTITION_PAGES,
           name='title available pages at most the title partition\'s')

    for size_name, size, protect, low in BLOCKS:
        block_scenario(corpus, 'title', size_name, size, protect, 1, low,
                       low + size - 1)

    # Type 2 from a title takes the system's memory, as XAM's own allocations
    # for a title do.
    block_scenario(corpus, 'system', '64 KB', 0x10000,
                   MEM_LARGE_PAGES | PAGE_READWRITE, 2, 0,
                   SYSTEM_PARTITION_END - 1)
