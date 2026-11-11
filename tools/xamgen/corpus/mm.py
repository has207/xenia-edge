"""Memory corpus: what committing a range that is partly committed does to its
pages, for 4 KB and 64 KB pages.

Each recommit scenario reserves three pages, commits them and fills them with a
pattern, decommits some of them and commits all three again.
RtlCompareMemoryUlong then counts how many bytes of each page still hold the
pattern, or read as zero for a page that was decommitted.

The ranged scenarios commit, decommit or reserve a range that starts in the
middle of a page, is empty or has size 0. They record the status, the base and
size the call returns and the state of each page. They also reset, release
part of a reservation and pass negative sizes. The bare scenarios make a call
with nothing reserved for it: a base below or above the title's address spaces,
base 0 or a negative size.
"""

import struct

MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_DECOMMIT = 0x4000
MEM_RELEASE = 0x8000
MEM_RESET = 0x80000
MEM_LARGE_PAGES = 0x20000000
PAGE_READWRITE = 4
PAGES = 3
PATTERN = 0x5AA5C33C

PAGE_SIZES = (('4 KB', 0x1000, 0), ('64 KB', 0x10000, MEM_LARGE_PAGES))
# The pages each scenario decommits, a contiguous run.
DECOMMITS = (('first', (0,)), ('middle', (1,)), ('last two', (1, 2)))


def build(corpus):
    for size_name, page, large in PAGE_SIZES:
        for which, decommitted in DECOMMITS:
            s = corpus.scenario(f'recommit {size_name} pages with the '
                                f'{which} decommitted')
            base = s.alloc(4, 'base', fill=0)
            size = s.alloc(4, 'size', struct.pack('>I', PAGES * page))
            region = s.variable(f'{size_name} region')
            s.call('NtAllocateVirtualMemory', base, size,
                   MEM_RESERVE | large, PAGE_READWRITE, 0, name='reserve',
                   store=[(region, base)])
            s.write(size, struct.pack('>I', PAGES * page))
            s.call('NtAllocateVirtualMemory', base, size, MEM_COMMIT,
                   PAGE_READWRITE, 0, name='commit')
            s.call('RtlFillMemoryUlong', region, PAGES * page, PATTERN,
                   name='fill', void=True)
            first = decommitted[0] * page
            s.call('RtlFillMemoryUlong', base, 4, region + first,
                   name='point at the decommit', void=True)
            s.write(size, struct.pack('>I', len(decommitted) * page))
            s.call('NtFreeVirtualMemory', base, size, MEM_DECOMMIT, 0,
                   name='decommit')
            s.call('RtlFillMemoryUlong', base, 4, region,
                   name='point at the region', void=True)
            s.write(size, struct.pack('>I', PAGES * page))
            s.call('NtAllocateVirtualMemory', base, size, MEM_COMMIT,
                   PAGE_READWRITE, 0, name='commit again')
            for i in range(PAGES):
                if i in decommitted:
                    s.call('RtlCompareMemoryUlong', region + i * page, page, 0,
                           name=f'page {i} zeroed')
                else:
                    s.call('RtlCompareMemoryUlong', region + i * page, page,
                           PATTERN, name=f'page {i} kept')
            s.write(size, struct.pack('>I', 0))
            s.call('NtFreeVirtualMemory', base, size, MEM_RELEASE, 0,
                   name='release')
    for size_name, page, large in PAGE_SIZES:
        for which, setup, export, flags, offset, length in ranges(page):
            ranged(corpus, which, size_name, page, large, setup, export, flags,
                   offset, length)
    for which, flags in (('commit', MEM_COMMIT), ('reserve', MEM_RESERVE)):
        bare(corpus, f'{which} at a base inside the first page', flags, 0x800)
    # The title's 4 KB pages end at 2FFF0000 and its 64 KB pages at 6FFF0000.
    for size_name, address in (('4 KB', 0x2FFF8000), ('64 KB', 0x6FFF8000)):
        for which, flags in (('commit', MEM_COMMIT),
                             ('decommit', MEM_DECOMMIT),
                             ('release', MEM_RELEASE)):
            bare(corpus, f'{which} at a base past the {size_name} pages',
                 flags, address)
    for which, flags in (('decommit', MEM_DECOMMIT), ('release', MEM_RELEASE)):
        bare(corpus, f'{which} at base 0', flags, 0)
    for size_name, large in (('4 KB', 0), ('64 KB', MEM_LARGE_PAGES)):
        bare(corpus, f'reserve {size_name} pages with a negative size',
             MEM_RESERVE | large, 0, -0x10000)


def ranges(page):
    """(name, setup, export, flags, base offset, length) for each ranged
    scenario. The setup leaves the three pages reserved, committed, or
    released so the call can reserve at its own base."""
    half = page // 2
    commit = ('NtAllocateVirtualMemory', MEM_COMMIT)
    decommit = ('NtFreeVirtualMemory', MEM_DECOMMIT)
    release = ('NtFreeVirtualMemory', MEM_RELEASE)
    return (
        ('commit from the middle of a page into the next', 'reserved',
         *commit, half, page),
        ('commit inside a page from its middle', 'reserved', *commit, half,
         0x10),
        ('decommit from the middle of a page into the next', 'committed',
         *decommit, half, page),
        ('decommit a page with size 0', 'committed', *decommit, page, 0),
        ('commit inside the last page from its middle', 'reserved', *commit,
         2 * page + half, 0x10),
        ('decommit inside the last page from its middle', 'committed',
         *decommit, 2 * page + half, 0x10),
        ('reserve from the middle of a page into the next', 'released',
         'NtAllocateVirtualMemory', MEM_RESERVE, page + half, page),
        ('reserve and commit from the middle of a page into the next',
         'released', 'NtAllocateVirtualMemory', MEM_RESERVE | MEM_COMMIT,
         page + half, page),
        ('commit from the last page past the end of a reservation',
         'reserved', *commit, 2 * page, 2 * page),
        ('decommit from the last page past the end of a reservation',
         'committed', *decommit, 2 * page, 2 * page),
        ('commit in a released region', 'released', *commit, page, page),
        ('decommit in a released region', 'released', *decommit, page, page),
        ('reserve over a reservation', 'reserved', 'NtAllocateVirtualMemory',
         MEM_RESERVE, page, page),
        ('reset committed pages', 'committed', 'NtAllocateVirtualMemory',
         MEM_RESET, page, page),
        ('reset in a released region', 'released', 'NtAllocateVirtualMemory',
         MEM_RESET, page, page),
        ('commit with a negative size', 'reserved', *commit, page, -page),
        ('decommit with a negative size', 'committed', *decommit, page,
         -page),
        ('release from the second page with size 0', 'reserved', *release,
         page, 0),
        ('release the first page', 'reserved', *release, 0, page),
        ('release the middle page', 'reserved', *release, page, page),
        ('release the last page', 'reserved', *release, 2 * page, page),
        ('release from the middle of a page into the next', 'reserved',
         *release, half, page),
    )


def ranged(corpus, which, size_name, page, large, setup, export, flags, offset,
           length):
    s = corpus.scenario(f'{which}, {size_name} pages')
    base = s.alloc(4, 'base', fill=0)
    size = s.alloc(4, 'size', struct.pack('>I', PAGES * page))
    info = s.alloc(0x1C, 'info', fill=0)
    region = s.variable(f'{size_name} region')
    s.call('NtAllocateVirtualMemory', base, size, MEM_RESERVE | large,
           PAGE_READWRITE, 0, name='reserve', store=[(region, base)])
    if setup == 'committed':
        s.write(size, struct.pack('>I', PAGES * page))
        s.call('NtAllocateVirtualMemory', base, size, MEM_COMMIT,
               PAGE_READWRITE, 0, name='commit')
    elif setup == 'released':
        s.write(size, struct.pack('>I', 0))
        s.call('NtFreeVirtualMemory', base, size, MEM_RELEASE, 0,
               name='release the reservation')
    if flags == MEM_RESET and setup == 'committed':
        s.call('RtlFillMemoryUlong', region, PAGES * page, PATTERN,
               name='fill', void=True)
    s.call('RtlFillMemoryUlong', base, 4, region + offset,
           name='point at the base', void=True)
    s.write(size, struct.pack('>I', length & 0xFFFFFFFF))
    if export == 'NtAllocateVirtualMemory':
        if flags & MEM_RESERVE:
            flags |= large
        args = (flags, PAGE_READWRITE, 0)
    else:
        args = (flags, 0)
    s.call(export, base, size, *args, name='range', capture=[size])
    for i in range(PAGES + 1):
        s.call('RtlCompareMemoryUlong', base, 4, region + i * page,
               name=f'base at page {i}')
    if flags == MEM_RESET and setup == 'committed':
        s.call('RtlCompareMemoryUlong', region + page, page, PATTERN,
               name='page 1 kept')
    # region_size and state; the rest holds addresses or differs by protect.
    # A free run's size depends on the address space layout so pages released
    # and not reserved again aren't queried. A partial release records only the
    # state.
    releasing = export == 'NtFreeVirtualMemory' and flags == MEM_RELEASE
    field = (info + 0x10).prefix(4) if releasing else (info + 0xC).prefix(8)
    for i in range(PAGES if setup != 'released' or flags & MEM_RESERVE else 0):
        s.call('NtQueryVirtualMemory', region + i * page, info, 0,
               name=f'page {i}', capture=[field])
    s.write(size, struct.pack('>I', 0))
    if releasing:
        s.call('RtlFillMemoryUlong', base, 4, region,
               name='point at the region', void=True)
        # Fails where the release took the first page or the whole region.
        s.call('NtFreeVirtualMemory', base, size, MEM_RELEASE, 0,
               name='release what is left', void=True)
        return
    if setup == 'released':
        # Frees whatever the call reserved; fails where it reserved nothing.
        s.call('NtFreeVirtualMemory', base, size, MEM_RELEASE, 0,
               name='release what it reserved', void=True)
        return
    s.call('RtlFillMemoryUlong', base, 4, region, name='point at the region',
           void=True)
    s.call('NtFreeVirtualMemory', base, size, MEM_RELEASE, 0, name='release')


def bare(corpus, name, flags, address, length=0x1000):
    """A call with no reservation set up for it."""
    s = corpus.scenario(name)
    base = s.alloc(4, 'base', struct.pack('>I', address))
    size = s.alloc(4, 'size', struct.pack('>I', length & 0xFFFFFFFF))
    if flags in (MEM_DECOMMIT, MEM_RELEASE):
        s.call('NtFreeVirtualMemory', base, size, flags, 0, name='range',
               capture=[size])
    else:
        s.call('NtAllocateVirtualMemory', base, size, flags, PAGE_READWRITE, 0,
               name='range', capture=[size])
    s.call('RtlCompareMemoryUlong', base, 4, address, name='base unchanged')
    # Frees whatever the call allocated; fails where it allocated nothing.
    s.write(size, struct.pack('>I', 0))
    s.call('NtFreeVirtualMemory', base, size, MEM_RELEASE, 0,
           name='release what it allocated', void=True)
