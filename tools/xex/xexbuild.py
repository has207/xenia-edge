#!/usr/bin/env python3
"""Build a minimal unencrypted XEX2 title from raw pages.

The image uses 64 KiB pages at a fixed load address. Page 0 holds a synthesized
PE header, the caller supplies the remaining pages with their section types
(1 code, 2 data, 3 read-only data). Imports come from an Imports layout whose
bytes the caller places at its base address in a code page.

Every hash is real: the page descriptor chain, the section digest and the
header digest. The RSA signature is zero, so the result only loads where
signature checks are off (RGH/JTAG) or in an emulator.

Hash rules, verified against retail XEX files:
  descriptor[i].digest = SHA1(page_block[i + 1] + descriptor[i + 1])
  descriptor[last].digest = 0
  section_digest = SHA1(page_block[0] + descriptor[0])
  header_digest = SHA1(xex[sec + 0x17C:header_size] + xex[0:sec + 8])
  library[i].next_import_digest = SHA1(library[i + 1][4:])
  library[last].next_import_digest = 0
  import_table_digest = SHA1(library[0][4:])

    from xexbuild import build
    xex = build(0x82000000, 0x82010000, [(1, code), (2, data)])
"""

import hashlib
import struct

PAGE_SIZE = 0x10000
HEADER_SIZE = 0x1000

SECTION_CODE = 1
SECTION_DATA = 2
SECTION_READONLY = 3

KEY_FILE_FORMAT_INFO = 0x000003FF
KEY_ENTRY_POINT = 0x00010100
KEY_IMAGE_BASE_ADDRESS = 0x00010201
KEY_IMPORT_LIBRARIES = 0x000103FF
KEY_DEFAULT_STACK_SIZE = 0x00020200
KEY_SYSTEM_FLAGS = 0x00030000
KEY_EXECUTION_INFO = 0x00040006

PE_SECTION_FLAGS = {
    SECTION_CODE: 0x60000020,      # code, execute, read
    SECTION_DATA: 0xC0000040,      # initialized data, read, write
    SECTION_READONLY: 0x40000040,  # initialized data, read
}
PE_SECTION_NAMES = {SECTION_CODE: b'.text', SECTION_DATA: b'.data',
                    SECTION_READONLY: b'.rdata'}
# The function entries the kernel's RtlLookupFunctionEntry searches.
PE_EXCEPTION_DIRECTORY = 3

XAM = 'xam.xex'
XBOXKRNL = 'xboxkrnl.exe'
# Every retail title carries these ids for the two system libraries.
LIBRARY_IDS = {XAM: 0xFCA15C76, XBOXKRNL: 0x45DC17E0}


def version(major, minor, build, qfe=0):
    return (major << 28) | (minor << 24) | (build << 8) | qfe


class Imports:
    """Import records laid out from `base`, an address in a code page.

    A function import is a slot the loader fills with the export's address
    followed by the 16-byte thunk it rewrites to jump there, so callers branch
    to the address function() returns. A variable import is a slot holding the
    variable's address once loaded. data() is what goes at `base`.
    """

    def __init__(self, base, built=version(2, 0, 17559),
                 minimum=version(2, 0, 9199)):
        self.base = base
        self.built = built
        self.minimum = minimum
        self._data = bytearray()
        self._records = {}  # library -> record addresses, in loader order
        self._cache = {}

    def _record(self, library, ordinal):
        if library not in LIBRARY_IDS:
            raise ValueError(f'no id for import library {library}')
        if library not in self._records:
            self._records[library] = []
        return (list(self._records).index(library) << 16) | ordinal

    def function(self, library, ordinal):
        key = (library, ordinal, 'function')
        if key not in self._cache:
            record = self._record(library, ordinal)
            slot = self.base + len(self._data)
            self._data += struct.pack('>IIIII', record, 0x01000000 | record,
                                      0x02000000 | record,
                                      0x7D6903A6,   # mtctr r11
                                      0x4E800420)   # bctr
            self._records[library] += [slot, slot + 4]
            self._cache[key] = slot + 4
        return self._cache[key]

    def variable(self, library, ordinal):
        key = (library, ordinal, 'variable')
        if key not in self._cache:
            record = self._record(library, ordinal)
            slot = self.base + len(self._data)
            self._data += struct.pack('>I', record)
            self._records[library].append(slot)
            self._cache[key] = slot
        return self._cache[key]

    def data(self):
        return bytes(self._data)

    def header(self):
        """The import libraries header and the security info's count and
        digest for it."""
        strings = b''
        for library in self._records:
            name = library.encode() + b'\0'
            strings += name + bytes(-len(name) % 4)
        records = []
        for index, (library, addresses) in enumerate(self._records.items()):
            record = bytearray(struct.pack(
                '>I20sIIIHH', 0x28 + 4 * len(addresses), bytes(20),
                LIBRARY_IDS[library], self.built, self.minimum, index,
                len(addresses)))
            record += b''.join(struct.pack('>I', a) for a in addresses)
            records.append(record)
        for i in range(len(records) - 2, -1, -1):
            records[i][4:24] = hashlib.sha1(records[i + 1][4:]).digest()
        body = strings + b''.join(records)
        header = struct.pack('>III', 12 + len(body), len(strings),
                             len(records)) + body
        digest = hashlib.sha1(records[0][4:]).digest() if records else bytes(20)
        return header, len(records), digest


def _pe_header(load_address, entry, page_types, directories):
    """PE headers for page 0, with one section per run of same-typed pages
    and the data directories `directories` maps from their index to an
    (address, size) pair."""
    runs = []
    for index, kind in enumerate(page_types, start=1):
        if runs and runs[-1][0] == kind:
            runs[-1][2] += 1
        else:
            runs.append([kind, index, 1])
    image_size = (len(page_types) + 1) * PAGE_SIZE
    code_size = sum(r[2] for r in runs if r[0] == SECTION_CODE) * PAGE_SIZE
    data_size = sum(r[2] for r in runs if r[0] != SECTION_CODE) * PAGE_SIZE
    code_base = next((r[1] for r in runs if r[0] == SECTION_CODE), 1) * PAGE_SIZE
    data_base = next((r[1] for r in runs if r[0] != SECTION_CODE), 1) * PAGE_SIZE

    page = bytearray(PAGE_SIZE)
    # Retail images have e_cblp 0x90 and Xenia only loads those.
    page[0:4] = b'MZ\x90\x00'
    e_lfanew = 0x80
    struct.pack_into('<I', page, 0x3C, e_lfanew)
    page[e_lfanew:e_lfanew + 4] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', page, e_lfanew + 4,
                     0x01F2,     # IMAGE_FILE_MACHINE_POWERPCBE
                     len(runs), 0, 0, 0,
                     0xE0,       # sizeof(IMAGE_OPTIONAL_HEADER32)
                     0x0102)     # executable, 32-bit machine
    optional = e_lfanew + 24
    struct.pack_into('<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII', page, optional,
                     0x10B, 10, 0,
                     code_size, data_size, 0,
                     entry - load_address, code_base, data_base,
                     load_address, PAGE_SIZE, 0x200,
                     0, 0, 0, 0, 1, 0, 0,
                     image_size, 0x400, 0,
                     0x0E,       # IMAGE_SUBSYSTEM_XBOX
                     0,
                     0x40000, 0x40000, 0x100000, 0x1000,
                     0, 16)
    for index, (address, size) in (directories or {}).items():
        struct.pack_into('<II', page, optional + 0x60 + 8 * index,
                         address - load_address, size)
    sections = optional + 0xE0
    for i, (kind, first, count) in enumerate(runs):
        struct.pack_into('<8sIIIIIIHHI', page, sections + 40 * i,
                         PE_SECTION_NAMES[kind], count * PAGE_SIZE,
                         first * PAGE_SIZE, count * PAGE_SIZE,
                         first * PAGE_SIZE, 0, 0, 0, 0,
                         PE_SECTION_FLAGS[kind])
    return bytes(page)


def build(load_address, entry, pages, title_id=0xFFFF7E57, stack_size=0x40000,
          imports=None, directories=None):
    """Returns the XEX2 file for `pages`, a list of (section type, bytes), with
    the import records of `imports` if given. `directories` maps a PE data
    directory's index, such as PE_EXCEPTION_DIRECTORY, to the guest address
    and size of its data."""
    page_types = [kind for kind, _ in pages]
    if imports:
        offset = imports.base - load_address - PAGE_SIZE
        kind, data = pages[offset // PAGE_SIZE]
        start = offset % PAGE_SIZE
        if (kind != SECTION_CODE or
                data[start:start + len(imports.data())] != imports.data()):
            raise ValueError('import records are not at their base in code')
    blocks = [_pe_header(load_address, entry, page_types, directories)]
    for kind, data in pages:
        if len(data) > PAGE_SIZE:
            raise ValueError('page data larger than a page')
        blocks.append(bytes(data) + bytes(PAGE_SIZE - len(data)))
    types = [SECTION_READONLY] + page_types
    image = b''.join(blocks)

    # Page descriptors, hashed back to front.
    descriptors = [bytearray(24) for _ in blocks]
    for i, kind in enumerate(types):
        struct.pack_into('>I', descriptors[i], 0, (1 << 4) | kind)
    for i in range(len(blocks) - 2, -1, -1):
        digest = hashlib.sha1(blocks[i + 1] + descriptors[i + 1]).digest()
        descriptors[i][4:24] = digest
    section_digest = hashlib.sha1(blocks[0] + descriptors[0]).digest()

    # Optional headers: inline values and two data blocks.
    file_format = struct.pack('>IHHII', 16, 0, 1, len(image), 0)
    execution_info = struct.pack('>IIIIBBBBI', 0, 0, 0, title_id, 0, 0, 1, 1, 0)
    headers = [
        (KEY_FILE_FORMAT_INFO, file_format),
        (KEY_ENTRY_POINT, entry),
        (KEY_IMAGE_BASE_ADDRESS, load_address),
        (KEY_DEFAULT_STACK_SIZE, stack_size),
        (KEY_SYSTEM_FLAGS, 0),
        (KEY_EXECUTION_INFO, execution_info),
    ]
    import_count, import_digest = 0, bytes(20)
    if imports:
        import_header, import_count, import_digest = imports.header()
        # Keys stay in ascending order like retail headers.
        headers.insert(3, (KEY_IMPORT_LIBRARIES, import_header))
    table_size = 0x18 + 8 * len(headers)
    data_offset = table_size
    table = bytearray()
    data_area = bytearray()
    for key, value in headers:
        if isinstance(value, bytes):
            table += struct.pack('>II', key, data_offset + len(data_area))
            data_area += value
            data_area += bytes(-len(data_area) % 8)
        else:
            table += struct.pack('>II', key, value)
    security_offset = (data_offset + len(data_area) + 0xF) & ~0xF

    security = bytearray(0x184 + 24 * len(descriptors))
    struct.pack_into('>II', security, 0, len(security), len(image))
    struct.pack_into('>III', security, 0x108, 0x174, 0, load_address)
    security[0x114:0x128] = section_digest
    struct.pack_into('>I', security, 0x128, import_count)
    security[0x12C:0x140] = import_digest
    struct.pack_into('>III', security, 0x178, 0xFFFFFFFF, 0xFFFFFFFF,
                     len(descriptors))
    for i, descriptor in enumerate(descriptors):
        security[0x184 + 24 * i:0x184 + 24 * (i + 1)] = descriptor
    if security_offset + len(security) > HEADER_SIZE:
        raise ValueError('headers do not fit in HEADER_SIZE')

    header = bytearray(HEADER_SIZE)
    struct.pack_into('>4sIIIII', header, 0, b'XEX2', 0x1, HEADER_SIZE, 0,
                     security_offset, len(headers))
    header[0x18:0x18 + len(table)] = table
    header[data_offset:data_offset + len(data_area)] = data_area
    header[security_offset:security_offset + len(security)] = security
    sec = security_offset
    header_digest = hashlib.sha1(bytes(header[sec + 0x17C:HEADER_SIZE]) +
                                 bytes(header[0:sec + 8])).digest()
    header[sec + 0x164:sec + 0x178] = header_digest
    return bytes(header) + image
