"""Physical memory corpus: MmAllocatePhysicalMemoryEx, MmAllocatePhysicalMemory,
MmFreePhysicalMemory, MmQueryAllocationSize, MmQueryAddressProtect and
MmGetPhysicalAddress, for 4 KB, 64 KB and 16 MB pages.

The ranged scenarios allocate into a window of physical addresses and record
the address the call returns, its physical address and the block's size. The
windows are as large as the block with the high end on a page boundary, a byte
short of one or inside a page, larger than the block, a single page, open at
the low end, start inside a page, have the low end above the high end or lie
above physical memory. Others pass an alignment above the page size, one that
isn't a multiple of it or a power of two, a size inside a page or larger than
the window, a protection the call accepts or rejects, or a partition type. The
placement scenarios allocate next to blocks already in the window, free a block
and allocate it again, free and query from inside a block, and reach a block
through the other windows onto its pages. The last ones ask for the top of
physical memory, which the console keeps from titles.

Every ranged scenario has its own 1 MB of physical memory from 256 MB up, or
16 MB from 384 MB up for 16 MB pages, well clear of the title's own pages. The
unranged scenarios take memory wherever the call puts it, so only what doesn't
depend on the layout is recorded: the region of the address, how far its
physical address is from it, its alignment where the call fixes it, and the
size.

MmQueryAllocationSize of an address outside physical memory walks the page
tables until it finds the end of an allocation, and MmFreePhysicalMemory of 0
frees whatever starts at physical page 0. Both go through routines that skip a
block that wasn't allocated.
"""

PAGE_NOACCESS = 0x01
PAGE_READONLY = 0x02
PAGE_READWRITE = 0x04
PAGE_EXECUTE = 0x10
PAGE_EXECUTE_READWRITE = 0x40
PAGE_GUARD = 0x100
PAGE_NOCACHE = 0x200
PAGE_WRITECOMBINE = 0x400
MEM_LARGE_PAGES = 0x20000000
MEM_16MB_PAGES = 0x80000000

PAGES = {'4 KB': PAGE_READWRITE,
         '64 KB': MEM_LARGE_PAGES | PAGE_READWRITE,
         '16 MB': MEM_16MB_PAGES | PAGE_READWRITE}

BASE = 0x10000000
SLOT = 0x100000
LARGE_BASE = 0x18000000
LARGE_SLOT = 0x1000000


class Slots:
    def __init__(self):
        self.small = BASE
        self.large = LARGE_BASE

    def take(self, size_name='4 KB'):
        if size_name == '16 MB':
            slot, self.large = self.large, self.large + LARGE_SLOT
            return slot
        slot, self.small = self.small, self.small + SLOT
        if self.small > LARGE_BASE:
            raise ValueError('out of 1 MB slots')
        return slot


class Routines:
    """The guards and reductions a scenario calls, made once each."""

    def __init__(self, s):
        self.s = s
        self.made = {}

    def get(self, name, emit):
        if name not in self.made:
            self.made[name] = self.s.routine(name, emit)
        return self.made[name]

    def size(self):
        """MmQueryAllocationSize(r3) if the block at r4 was allocated, else
        0."""
        def emit(a, r, thunk):
            a.label(r.entry)
            a.cmplwi(4, 0)
            a.beq(r.label('none'))
            a.load32(12, thunk('MmQueryAllocationSize'))
            a.mtctr(12)
            a.bctr()
            a.label(r.label('none'))
            a.li(3, 0)
            a.blr()
        return self.get('size if allocated', emit)

    def protect(self):
        """MmQueryAddressProtect(r3) if the block at r4 was allocated, else
        0. Address 0 would report the title's own first page."""
        def emit(a, r, thunk):
            a.label(r.entry)
            a.cmplwi(4, 0)
            a.beq(r.label('none'))
            a.load32(12, thunk('MmQueryAddressProtect'))
            a.mtctr(12)
            a.bctr()
            a.label(r.label('none'))
            a.li(3, 0)
            a.blr()
        return self.get('protect if allocated', emit)

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

    def region(self):
        """The top 3 bits of r3, the region an address is in."""
        def emit(a, r, thunk):
            a.label(r.entry)
            a.rlwinm(3, 3, 0, 0, 2)
            a.blr()
        return self.get('region', emit)

    def low16(self):
        """The low 16 bits of r3."""
        def emit(a, r, thunk):
            a.label(r.entry)
            a.rlwinm(3, 3, 0, 16, 31)
            a.blr()
        return self.get('low 16 bits', emit)

    def skew(self):
        """Physical address r3 less the low 29 bits of its address r4."""
        def emit(a, r, thunk):
            a.label(r.entry)
            a.rlwinm(4, 4, 0, 3, 31)
            a.subf(3, 4, 3)
            a.blr()
        return self.get('physical offset', emit)


def allocate(s, name, size, protect, low, high, alignment, type_=0):
    block = s.variable(name)
    s.call('MmAllocatePhysicalMemoryEx', type_, size, protect, low, high,
           alignment, name=f'allocate {name}', store=[(block, 'r3')])
    return block


def describe(s, routines, block, name):
    """The block's physical address and size."""
    s.call('MmGetPhysicalAddress', block, name=f'physical address of {name}')
    s.call(routines.size(), block, block, name=f'size of {name}')


def ranged(corpus, slots, which, size_name, size, low, high, alignment=0,
           protect=None, type_=0, query_protect=False):
    slot = slots.take(size_name)
    s = corpus.scenario(f'{which}, {size_name} pages')
    routines = Routines(s)
    if protect is None:
        protect = PAGES[size_name]
    block = allocate(s, 'block', size, protect, slot + low, slot + high,
                     alignment, type_)
    describe(s, routines, block, 'block')
    if query_protect:
        s.call(routines.protect(), block, block, name='protect of block')
    return s, routines, block


def build(corpus):
    slots = Slots()

    # The window's bounds.
    for which, size, low, high, alignment in (
            ('window as large as the block ending on a page boundary',
             0x10000, 0, 0x10000, 0),
            ('window as large as the block ending a byte short of a page',
             0x10000, 0, 0xFFFF, 0),
            ('window larger than the block', 0x10000, 0, 0x40000, 0),
            ('window starting inside a page', 0x10000, 0x800, 0xFFFF, 0),
            ('alignment above the page size', 0x1000, 0, 0x10000, 0x10000),
            ('low end above the high end', 0x1000, 0x10000, 0, 0)):
        ranged(corpus, slots, which, '4 KB', size, low, high, alignment)
    for which, size, low, high in (
            ('window as large as the block ending on a page boundary',
             0x10000, 0, 0x10000),
            ('window a page larger than the block', 0x10000, 0, 0x20000)):
        ranged(corpus, slots, which, '64 KB', size, low, high)
    for which, size, low, high in (
            ('window ending inside a page', 0x10000, 0, 0x10800),
            ('window of one page', 0x1000, 0, 0),
            ('window of one page for two pages', 0x2000, 0, 0),
            ('size larger than the window', 0x20000, 0, 0x10000),
            ('size inside a page', 0x1800, 0, 0x10000)):
        ranged(corpus, slots, which, '4 KB', size, low, high)
    ranged(corpus, slots, 'size of one page', '64 KB', 0x1000, 0, 0x10000)

    # A window with no low end, its block at the top of the slot.
    slot = slots.take()
    s = corpus.scenario('window with no low end, 4 KB pages')
    routines = Routines(s)
    block = allocate(s, 'block', 0x10000, PAGE_READWRITE, 0, slot + 0x10000, 0)
    describe(s, routines, block, 'block')

    # Above the 512 MB of physical memory, so no slot.
    for size_name in ('4 KB', '64 KB'):
        s = corpus.scenario(f'window above physical memory, {size_name} '
                            'pages')
        routines = Routines(s)
        block = allocate(s, 'block', 0x1000, PAGES[size_name], 0x30000000,
                         0x30010000, 0)
        describe(s, routines, block, 'block')

    # Alignment.
    for which, size, high, alignment in (
            ('alignment that is not a multiple of the page size', 0x1000,
             0x11000, 0x1800),
            ('alignment that is not a power of two', 0x1000, 0x12000, 0x3000),
            ('alignment and size of 64 KB in a window a page larger', 0x10000,
             0x11000, 0x10000),
            ('alignment of 4 bytes', 0x10000, 0x10000, 4)):
        ranged(corpus, slots, which, '4 KB', size, 0, high, alignment)
    ranged(corpus, slots, 'alignment below the page size', '64 KB', 0x10000,
           0, 0x20000, 0x1000)

    # 16 MB pages.
    ranged(corpus, slots,
           'window as large as the block ending on a page boundary', '16 MB',
           0x1000000, 0, 0x1000000)
    ranged(corpus, slots, 'size of one page', '16 MB', 0x1000, 0, 0x1000000)

    # Protection, each in a one page window that ends on a page boundary.
    for which, protect in (
            ('read-only', PAGE_READONLY),
            ('read-write', PAGE_READWRITE),
            ('read-only write-combined', PAGE_READONLY | PAGE_WRITECOMBINE),
            ('read-write write-combined', PAGE_READWRITE | PAGE_WRITECOMBINE),
            ('read-write uncached', PAGE_READWRITE | PAGE_NOCACHE),
            ('no protection', 0),
            ('no access', PAGE_NOACCESS),
            ('execute read-write', PAGE_EXECUTE_READWRITE),
            ('read-write and execute', PAGE_READWRITE | PAGE_EXECUTE),
            ('read-only and read-write', PAGE_READONLY | PAGE_READWRITE),
            ('read-write guard', PAGE_READWRITE | PAGE_GUARD),
            ('read-write uncached and write-combined',
             PAGE_READWRITE | PAGE_NOCACHE | PAGE_WRITECOMBINE)):
        ranged(corpus, slots, f'protection {which}', '4 KB', 0x1000, 0,
               0x1000, protect=protect, query_protect=True)
    # A window a 64 KB page fits in, so taking either page size shows.
    ranged(corpus, slots, 'protection with both large page sizes', '4 KB',
           0x1000, 0, 0x10000,
           protect=MEM_16MB_PAGES | MEM_LARGE_PAGES | PAGE_READWRITE,
           query_protect=True)
    for size_name in ('64 KB', '16 MB'):
        page = 0x10000 if size_name == '64 KB' else 0x1000000
        ranged(corpus, slots, 'protection read-write write-combined',
               size_name, page, 0, page,
               protect=PAGES[size_name] | PAGE_WRITECOMBINE,
               query_protect=True)

    # The partition type. A system block is freed, the title's go with it.
    for which, type_ in (('title type', 1), ('system type', 2),
                         ('type past the last', 3)):
        s, routines, block = ranged(corpus, slots, which, '4 KB', 0x1000, 0,
                                    0x1000, type_=type_)
        if type_ == 2:
            s.call(routines.free(), 2, block, block, name='free block',
                   void=True)

    # Blocks next to blocks already in the window.
    slot = slots.take()
    s = corpus.scenario('blocks top-down under blocks, 4 KB pages')
    routines = Routines(s)
    window = (slot, slot + 0x40000)
    first = allocate(s, 'first', 0x10000, PAGE_READWRITE, *window, 0)
    second = allocate(s, 'second', 0x10000, PAGE_READWRITE, *window, 0)
    describe(s, routines, first, 'first')
    describe(s, routines, second, 'second')
    s.call(routines.free(), 0, first, first, name='free first', void=True)
    third = allocate(s, 'third', 0x8000, PAGE_READWRITE, *window, 0)
    fourth = allocate(s, 'fourth', 0x10000, PAGE_READWRITE, *window, 0)
    describe(s, routines, third, 'third')
    describe(s, routines, fourth, 'fourth')

    slot = slots.take()
    s = corpus.scenario('64 KB pages skip a group a 4 KB block took')
    routines = Routines(s)
    small = allocate(s, 'small', 0x1000, PAGE_READWRITE, slot + 0x10000,
                     slot + 0x11000, 0)
    large = allocate(s, 'large', 0x10000, PAGES['64 KB'], slot, slot + 0x20000,
                     0)
    again = allocate(s, 'again', 0x10000, PAGES['64 KB'], slot, slot + 0x20000,
                     0)
    describe(s, routines, small, 'small')
    describe(s, routines, large, 'large')
    describe(s, routines, again, 'again')

    # A block in the same window twice, and again after it's freed.
    slot = slots.take()
    s = corpus.scenario('allocate a full window again before and after '
                        'freeing it, 4 KB pages')
    routines = Routines(s)
    window = (slot, slot + 0x10000)
    block = allocate(s, 'block', 0x10000, PAGE_READWRITE, *window, 0)
    taken = allocate(s, 'taken', 0x10000, PAGE_READWRITE, *window, 0)
    s.call(routines.free(), 0, block, block, name='free block', void=True)
    s.call(routines.size(), block, block, name='size of block after the free')
    s.call('MmGetPhysicalAddress', block,
           name='physical address of block after the free')
    s.call(routines.protect(), block, block,
           name='protect of block after the free')
    again = allocate(s, 'again', 0x10000, PAGE_READWRITE, *window, 0)
    describe(s, routines, again, 'again')

    # Inside a block.
    for size_name in ('4 KB', '64 KB'):
        slot = slots.take()
        s = corpus.scenario(f'free and query inside a block, {size_name} '
                            'pages')
        routines = Routines(s)
        block = allocate(s, 'block', 0x20000, PAGES[size_name], slot,
                         slot + 0x20000, 0)
        s.call('MmGetPhysicalAddress', block + 0x1234,
               name='physical address 0x1234 in')
        s.call(routines.size(), block + 0x10, block, name='size 0x10 in')
        s.call(routines.size(), block + 0x1000, block, name='size a page in')
        s.call(routines.size(), block + 0x10000, block,
               name='size 64 KB in')
        s.call(routines.free(), 0, block + 0x10000, block,
               name='free 64 KB in', void=True)
        s.call(routines.size(), block, block, name='size after the free in it')
        s.call(routines.free(), 0, block, block, name='free block', void=True)
        s.call(routines.free(), 0, block, block, name='free block again',
               void=True)
        s.call(routines.size(), block, block, name='size after the frees')

    # The shape of a pool: a large block filling its window exactly.
    slot = slots.take('16 MB')
    s = corpus.scenario('7 MB block filling its window with 4 byte alignment, '
                        '4 KB pages')
    routines = Routines(s)
    block = allocate(s, 'block', 0x680000, PAGE_READWRITE, slot,
                     slot + 0x680000, 4)
    describe(s, routines, block, 'block')

    # Unranged, reduced to what doesn't depend on the layout.
    for size_name, size, alignment, export in (
            ('4 KB', 0x1000, 0, 'MmAllocatePhysicalMemory'),
            ('4 KB', 0x1000, 0x10000, 'MmAllocatePhysicalMemoryEx'),
            ('4 KB', 0x1800, 0, 'MmAllocatePhysicalMemoryEx'),
            ('64 KB', 0x1000, 0, 'MmAllocatePhysicalMemory'),
            ('64 KB', 0x10000, 0x40000, 'MmAllocatePhysicalMemoryEx'),
            ('16 MB', 0x1000, 0, 'MmAllocatePhysicalMemory')):
        s = corpus.scenario(f'unranged {export} of {size:#x} with alignment '
                            f'{alignment:#x}, {size_name} pages')
        routines = Routines(s)
        block = s.variable('block')
        physical = s.variable('physical')
        if export == 'MmAllocatePhysicalMemory':
            s.call(export, 0, size, PAGES[size_name], name='allocate',
                   store=[(block, 'r3')], void=True)
        else:
            s.call(export, 0, size, PAGES[size_name], 0, 0xFFFFFFFF,
                   alignment, name='allocate', store=[(block, 'r3')],
                   void=True)
        s.call('MmGetPhysicalAddress', block, name='physical address',
               store=[(physical, 'r3')], void=True)
        s.call(routines.region(), block, name='region')
        s.call(routines.skew(), physical, block,
               name='physical address less the address')
        # Where in 64 KB a block lands is the layout's, unless the alignment
        # or the page size fixes it.
        if alignment >= 0x10000 or size_name != '4 KB':
            s.call(routines.low16(), block, name='low 16 bits of the address')
            s.call(routines.low16(), physical,
                   name='low 16 bits of the physical address')
        s.call(routines.size(), block, block, name='size')
        s.call(routines.protect(), block, block, name='protect')

    # An alignment that isn't a power of two masks the start with it less
    # one, so 3 pages only clear bit 1 of the page.
    ranged(corpus, slots, 'alignment that is not a power of two keeping bit 0',
           '4 KB', 0x1000, 0, 0x13000, 0x3000)
    slot = slots.take()
    s = corpus.scenario('alignment that is not a power of two under a block, '
                        '4 KB pages')
    routines = Routines(s)
    blocker = allocate(s, 'blocker', 0x1000, PAGE_READWRITE, slot + 0x11000,
                       slot + 0x11000, 0)
    block = allocate(s, 'block', 0x2000, PAGE_READWRITE, slot, slot + 0x13000,
                     0x3000)
    describe(s, routines, blocker, 'blocker')
    describe(s, routines, block, 'block')

    # A block through the other windows onto its pages.
    for size_name, others in (
            ('4 KB', (('64 KB', 0xA0000000), ('16 MB', 0xC0000000))),
            ('64 KB', (('4 KB', 0xDFFFF000),))):
        slot = slots.take()
        s = corpus.scenario(f'a block of {size_name} pages through the other '
                            'windows')
        routines = Routines(s)
        block = allocate(s, 'block', 0x10000, PAGES[size_name], slot,
                         slot + 0x10000, 0)
        physical = s.variable('physical')
        s.call('MmGetPhysicalAddress', block, name='physical address',
               store=[(physical, 'r3')])
        for other, window in others:
            alias = physical + window
            s.call(routines.size(), alias, block,
                   name=f'size through the {other} window')
            s.call('MmGetPhysicalAddress', alias,
                   name=f'physical address through the {other} window')
            s.call(routines.protect(), alias, block,
                   name=f'protect through the {other} window')
        other, window = others[0]
        s.call(routines.free(), 0, physical + window, block,
               name=f'free through the {other} window', void=True)
        s.call(routines.size(), block, block, name='size after the free')
        again = allocate(s, 'again', 0x10000, PAGES[size_name], slot,
                         slot + 0x10000, 0)
        describe(s, routines, again, 'again')

    # The 16 MB window maps all of physical memory read-only, and a block of
    # 16 MB pages sets its own protection there until it's freed.
    slot = slots.take()
    s = corpus.scenario('the 16 MB window onto memory nothing allocated')
    for which, physical in (('a title page', slot),
                            ('a system page', 0x00100000)):
        s.call('MmGetPhysicalAddress', 0xC0000000 + physical,
               name=f'physical address of {which}')
        s.call('MmQueryAddressProtect', 0xC0000000 + physical,
               name=f'protect of {which}')
    s.call('MmQueryAllocationSize', 0xC0000000 + slot,
           name='size of a title page')

    slot = slots.take('16 MB')
    s = corpus.scenario('a block of 16 MB pages back to read-only after its '
                        'free')
    routines = Routines(s)
    block = allocate(s, 'block', 0x1000000,
                     PAGES['16 MB'] | PAGE_WRITECOMBINE, slot,
                     slot + 0x1000000, 0)
    s.call(routines.protect(), block, block, name='protect of block')
    s.call(routines.free(), 0, block, block, name='free block', void=True)
    s.call('MmGetPhysicalAddress', 0xC0000000 + slot,
           name='physical address after the free')
    s.call('MmQueryAddressProtect', 0xC0000000 + slot,
           name='protect after the free')

    # The top of physical memory, which the console keeps from titles, and the
    # 4 KB pages past the highest one the call hands out.
    for which, size_name, low, high in (
            ('the top of physical memory', '64 KB', 0x1FCB0000, 0x1FCBFFFF),
            ('the top of physical memory', '4 KB', 0x1FFF0000, 0x1FFF0FFF),
            ('past the highest page', '4 KB', 0x1FFF1000, 0x1FFF1FFF)):
        s = corpus.scenario(f'{which}, {size_name} pages')
        routines = Routines(s)
        size = 0x10000 if size_name == '64 KB' else 0x1000
        block = allocate(s, 'block', size, PAGES[size_name], low, high, 0)
        describe(s, routines, block, 'block')

    # An alignment of three large pages masks the start with 0x2F pages, which
    # keeps it on a 64 KB page while leaving bit 4 of the page to the search.
    slot = slots.take()
    s = corpus.scenario('alignment of three pages, 64 KB pages')
    routines = Routines(s)
    first = allocate(s, 'first', 0x10000, PAGES['64 KB'], slot, slot + 0x80000,
                     0x30000)
    second = allocate(s, 'second', 0x10000, PAGES['64 KB'], slot,
                      slot + 0x80000, 0x30000)
    describe(s, routines, first, 'first')
    describe(s, routines, second, 'second')
