"""Memory corpus: what committing a range that is partly committed does to its
pages, for 4 KB and 64 KB pages.

Each scenario reserves three pages, commits them and fills them with a pattern,
decommits some of them and commits all three again. RtlCompareMemoryUlong then
counts how many bytes of each page still hold the pattern, or read as zero for
a page that was decommitted.
"""

import struct

MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_DECOMMIT = 0x4000
MEM_RELEASE = 0x8000
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
