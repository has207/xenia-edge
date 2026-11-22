"""Encrypted memory corpus: NtAllocateEncryptedMemory and NtFreeEncryptedMemory.

The free scenarios allocate a block and free it from its base, from an address
inside it or just below the next block, twice, with a region type other than
the caller's, or at an address outside the regions. The allocate scenarios pass
a bad type or size, take the whole region, and allocate again after frees and
next to a gap at the region's start. The base an allocation returns is
captured, as it names the region the type picked and where in it the block
went.
"""

PAGE = 0x10000
# The title region's size.
REGION = 0xFF0000
# Outside both regions a title can name.
OUTSIDE = (('at 0', 0), ('below the regions', 0x8BFF0000),
           ('above the regions', 0x8E000000))


def allocate(s, size, name):
    base = s.alloc(4, f'{name} base', fill=0)
    block = s.variable(name)
    s.call('NtAllocateEncryptedMemory', 0, size, base,
           name=f'allocate {size:#x}', capture=[base], store=[(block, base)])
    return block


def build(corpus):
    s = corpus.scenario('free at the base')
    block = allocate(s, 0x14, 'block')
    s.call('NtFreeEncryptedMemory', 0, block, name='free')
    s.call('NtFreeEncryptedMemory', 0, block, name='free again')

    s = corpus.scenario('free inside one page')
    block = allocate(s, 0x14, 'block')
    s.call('NtFreeEncryptedMemory', 0, block + 0x10, name='free 0x10 in')
    s.call('NtFreeEncryptedMemory', 0, block, name='free at the base')

    s = corpus.scenario('free inside two pages')
    block = allocate(s, 2 * PAGE, 'block')
    s.call('NtFreeEncryptedMemory', 0, block + PAGE,
           name='free at the second page')
    s.call('NtFreeEncryptedMemory', 0, block + 0x1000,
           name='free 0x1000 in')
    s.call('NtFreeEncryptedMemory', 0, block + 0x10, name='free 0x10 in')
    s.call('NtFreeEncryptedMemory', 0, block, name='free at the base after')

    s = corpus.scenario('free just below the next block')
    first = allocate(s, 0x14, 'block')
    second = allocate(s, 0x14, 'second block')
    s.call('NtFreeEncryptedMemory', 0, second + -0x10,
           name='free 0x10 below the second')
    s.call('NtFreeEncryptedMemory', 0, first, name='free the first')
    s.call('NtFreeEncryptedMemory', 0, second, name='free the second')

    s = corpus.scenario('free with another type')
    block = allocate(s, 0x14, 'block')
    s.call('NtFreeEncryptedMemory', 3, block, name='free with type 3')
    s.call('NtFreeEncryptedMemory', 2, block, name='free with type 2')
    s.call('NtFreeEncryptedMemory', 1, block, name='free with type 1')
    s.call('NtFreeEncryptedMemory', 0, block, name='free again with type 0')

    s = corpus.scenario('free outside the regions')
    for name, address in OUTSIDE:
        s.call('NtFreeEncryptedMemory', 0, address, name=f'free {name}')

    s = corpus.scenario('allocate with a bad type or size')
    for name, region_type, size in (
            ('allocate with type 3', 3, 0x14),
            ('allocate size 0', 0, 0),
            ('allocate a size past the region', 0, REGION + 1),
            ('allocate a size near 4 GB', 0, 0xFFFFFFF0)):
        # Filled so a base the call leaves alone shows.
        base = s.alloc(4, f'{name} base')
        s.call('NtAllocateEncryptedMemory', region_type, size, base,
               name=name, capture=[base])

    s = corpus.scenario('allocate the whole region')
    block = allocate(s, REGION, 'block')
    s.call('NtFreeEncryptedMemory', 0, block, name='free')

    s = corpus.scenario('allocate after freeing the first block')
    first = allocate(s, 0x14, 'block')
    second = allocate(s, 0x14, 'second block')
    s.call('NtFreeEncryptedMemory', 0, first, name='free the first')
    third = allocate(s, 0x14, 'third block')
    s.call('NtFreeEncryptedMemory', 0, second, name='free the second')
    s.call('NtFreeEncryptedMemory', 0, third, name='free the third')

    s = corpus.scenario('allocate after freeing the last block')
    first = allocate(s, 0x14, 'block')
    second = allocate(s, 0x14, 'second block')
    third = allocate(s, 0x14, 'third block')
    s.call('NtFreeEncryptedMemory', 0, first, name='free the first')
    s.call('NtFreeEncryptedMemory', 0, third, name='free the third')
    fourth = allocate(s, 0x14, 'fourth block')
    s.call('NtFreeEncryptedMemory', 0, second, name='free the second')
    s.call('NtFreeEncryptedMemory', 0, fourth, name='free the fourth')

    # A block that fills the region after its first page leaves a gap at the
    # start exactly one page long.
    s = corpus.scenario('allocate into a gap at the start of the same size')
    first = allocate(s, PAGE, 'block')
    rest = allocate(s, REGION - PAGE, 'second block')
    s.call('NtFreeEncryptedMemory', 0, first, name='free the first')
    third = allocate(s, PAGE, 'third block')
    s.call('NtFreeEncryptedMemory', 0, third, name='free the third')
    s.call('NtFreeEncryptedMemory', 0, rest, name='free the second')

    s = corpus.scenario('allocate into a larger gap at the start')
    first = allocate(s, 2 * PAGE, 'block')
    rest = allocate(s, REGION - 2 * PAGE, 'second block')
    s.call('NtFreeEncryptedMemory', 0, first, name='free the first')
    third = allocate(s, PAGE, 'third block')
    s.call('NtFreeEncryptedMemory', 0, third, name='free the third')
    s.call('NtFreeEncryptedMemory', 0, rest, name='free the second')
