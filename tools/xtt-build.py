#!/usr/bin/env python3

# Copyright 2026 Ben Vanik. All Rights Reserved.

"""Builds an Xbox 360 system font (.xtt) from TrueType fonts.

assets/system_font/XenonJKLatin.xtt stands in for the console's flash font and
comes from the bundled fonts:

  python3 tools/xtt-build.py assets/font/MPLUS1p-Regular.ttf \\
      assets/system_font/XenonJKLatin.xtt --name "Xbox JK" \\
      --latin "assets/font/Inter-VariableFont_opsz,wght.ttf" \\
      --asc 2075 --desc -410

Needs fontTools. The file it writes is laid out like the retail fonts:

  0x000 'xttf', then the 0x100-byte RSA signature of the rest
  0x104 u32 signed length, from 0x104 up to the first glyph page
  0x108 u32 file size, 0x10C u32 packed length, 0x110 u32 unpacked length
  0x114 u32 version 0x00010000
  0x118 zlib of an sfnt holding cmap head hhea hmtx name xchk xttf xloc, whose
        xglf record points at the glyph pages in the file
  4 KiB glyph pages, each one zlib stream of whole glyf records

xloc[i] is page << 16 | offset in the unpacked page, with an entry for the end
after the last glyph. xchk is the SHA-1 of each page. The xttf table is
u32 0x00010000, the largest unpacked page and the 0x100-byte signature of the
font a .xttp patch applies to (zeros in a base). A patch only attaches to a
base carrying that signature, which --bind copies from a patch.
"""

import argparse
import hashlib
import struct
import zlib

from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.scaleUpem import scale_upem
from fontTools.ttLib.tables import ttProgram
from fontTools.ttLib.tables._c_m_a_p import cmap_format_4
from fontTools.varLib.instancer import instantiateVariableFont

PAGE = 0x1000
# Under the largest unpacked page in the retail fonts (13494).
MAX_UNPACKED_PAGE = 0x3400
UNITS_PER_EM = 2048
LATIN_RANGES = [(0x20, 0x24F), (0x370, 0x3FF), (0x400, 0x4FF),
                (0x2000, 0x206F), (0x20A0, 0x20CF), (0x2100, 0x214F)]


def load(path, wght, opsz):
  font = TTFont(path)
  if 'fvar' in font:
    axes = {a.axisTag: a.defaultValue for a in font['fvar'].axes}
    axes.update({k: v for k, v in (('wght', wght), ('opsz', opsz))
                 if k in axes})
    font = instantiateVariableFont(font, axes)
  if font['head'].unitsPerEm != UNITS_PER_EM:
    scale_upem(font, UNITS_PER_EM)
  return font


def merge_latin(base, latin):
  """Maps LATIN_RANGES to the latin font's glyphs, decomposed so no component
  refers to a glyph of the other font."""
  latin_cmap = latin.getBestCmap()
  cmap = base.getBestCmap()
  glyph_set = latin.getGlyphSet()
  order = base.getGlyphOrder()
  added = {}
  for cp, name in sorted(latin_cmap.items()):
    if not any(a <= cp <= b for a, b in LATIN_RANGES):
      continue
    if name not in added:
      recording = DecomposingRecordingPen(glyph_set)
      glyph_set[name].draw(recording)
      pen = TTGlyphPen(None)
      recording.replay(pen)
      new_name = f'latin.{name}'
      base['glyf'][new_name] = pen.glyph()
      base['hmtx'][new_name] = latin['hmtx'][name]
      added[name] = new_name
    cmap[cp] = added[name]
  base.setGlyphOrder(order)
  base['maxp'].numGlyphs = len(order)
  return cmap


def glyph_records(font):
  """The glyf records in glyph order, without hinting, which the console's
  rasterizer doesn't run."""
  glyf = font['glyf']
  records = []
  for name in font.getGlyphOrder():
    glyph = glyf[name]
    if glyph.isComposite():
      if hasattr(glyph, 'program'):
        del glyph.program
    elif glyph.numberOfContours > 0:
      glyph.program = ttProgram.Program()
      glyph.program.fromBytecode(b'')
    data = glyph.compile(glyf) if glyph.numberOfContours else b''
    if len(data) % 2:
      data += b'\0'
    records.append(data)
  return records


def pack_pages(records):
  """Packs whole records into pages that compress to at most PAGE bytes."""
  pages, xloc = [], []
  page = b''
  for record in records:
    grown = page + record
    if page and (len(grown) > MAX_UNPACKED_PAGE or
                 len(zlib.compress(grown, 9)) > PAGE):
      pages.append(page)
      grown = record
    xloc.append(len(pages) << 16 | len(grown) - len(record))
    page = grown
  pages.append(page)
  xloc.append((len(pages) - 1) << 16 | len(page))
  return pages, xloc


def build_cmap(font, cmap):
  subtable = cmap_format_4(4)
  subtable.platformID, subtable.platEncID, subtable.language = 3, 1, 0
  subtable.cmap = {cp: g for cp, g in cmap.items() if cp <= 0xFFFF}
  table = newTable('cmap')
  table.tableVersion = 0
  table.tables = [subtable]
  return table.compile(font)


def patch_binding(path):
  """The signature of the base a .xttp patch applies to."""
  data = open(path, 'rb').read()
  packed = struct.unpack('>I', data[0x10C:0x110])[0]
  sfnt = zlib.decompress(data[0x118:0x118 + packed])
  for i in range(struct.unpack('>H', sfnt[4:6])[0]):
    record = sfnt[12 + 16 * i:28 + 16 * i]
    if record[:4] == b'xttf':
      _, offset, _ = struct.unpack('>III', record[4:])
      return sfnt[offset + 8:offset + 0x108]
  raise ValueError(f'{path} has no xttf table')


def build_name(names):
  """A name table of |names|, (name ID, string) pairs for Windows English."""
  strings = [string.encode('utf-16-be') for _, string in names]
  data = struct.pack('>3H', 0, len(names), 6 + 12 * len(names))
  offset = 0
  for (name_id, _), string in zip(names, strings):
    data += struct.pack('>6H', 3, 1, 0x409, name_id, len(string), offset)
    offset += len(string)
  return data + b''.join(strings)


def build(font, cmap, names, ascent, descent, signature):
  pages, xloc = pack_pages(glyph_records(font))
  packed_pages = [zlib.compress(page, 9) for page in pages]
  if max(map(len, packed_pages)) > PAGE:
    raise ValueError('a glyph is too big for a page')
  packed_pages = [page.ljust(PAGE, b'\0') for page in packed_pages]
  glyph_data = b''.join(packed_pages)
  order = font.getGlyphOrder()
  head, hhea, hmtx = font['head'], font['hhea'], font['hmtx']
  ascent = hhea.ascent if ascent is None else ascent
  descent = hhea.descent if descent is None else descent

  tables = {
      'cmap': build_cmap(font, cmap),
      'head': struct.pack('>IIIIHHqqhhhhHHhhh', 0x00010000, 0x00010000, 0,
                          0x5F0F3CF5, head.flags, UNITS_PER_EM, 0, 0,
                          head.xMin, head.yMin, head.xMax, head.yMax,
                          head.macStyle, head.lowestRecPPEM, 2, 1, 0),
      'hhea': struct.pack('>IhhhHhhhhhhhhhhhH', 0x00010000, ascent, descent,
                          0, max(hmtx[g][0] for g in order),
                          hhea.minLeftSideBearing, hhea.minRightSideBearing,
                          hhea.xMaxExtent, 1, 0, 0, 0, 0, 0, 0, 0,
                          len(order)),
      'hmtx': b''.join(struct.pack('>Hh', *hmtx[g]) for g in order),
      'name': build_name(names),
      'xchk': b''.join(hashlib.sha1(page).digest() for page in packed_pages),
      'xttf': struct.pack('>II', 0x00010000, max(map(len, pages))) +
              bytes(0x100),
      'xloc': struct.pack(f'>{len(xloc)}I', *xloc),
  }
  tags = list(tables) + ['xglf']

  def sfnt(xglf_offset):
    count = len(tags)
    selector = count.bit_length() - 1
    search_range = (1 << selector) * 16
    directory = struct.pack('>IHHHH', 0x00010000, count, search_range,
                            selector, count * 16 - search_range)
    body = b''
    offset = 12 + 16 * count
    for tag in tags:
      if tag == 'xglf':
        directory += tag.encode() + struct.pack('>III', 0, xglf_offset,
                                                len(glyph_data))
        continue
      data = tables[tag]
      directory += tag.encode() + struct.pack('>III', 0, offset, len(data))
      data += bytes(-len(data) % 4)
      body += data
      offset += len(data)
    return directory + body

  # The packed sfnt holds the offset of the pages that follow it.
  xglf_offset = PAGE
  while True:
    unpacked = sfnt(xglf_offset)
    packed = zlib.compress(unpacked, 9)
    needed = (0x118 + len(packed) + PAGE - 1) // PAGE * PAGE
    if needed == xglf_offset:
      break
    xglf_offset = needed
  header = b'xttf' + signature + struct.pack(
      '>5I', xglf_offset - 0x104, xglf_offset + len(glyph_data),
      xglf_offset - 0x118, len(unpacked), 0x00010000)
  return (header + packed.ljust(xglf_offset - 0x118, b'\0') + glyph_data,
          len(order), len(pages))


def main():
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument('source', help='TrueType font for every glyph')
  parser.add_argument('output', help='.xtt to write')
  parser.add_argument('--name', required=True, help='family name')
  parser.add_argument('--latin', help='TrueType font for Latin, Greek and '
                      'Cyrillic')
  parser.add_argument('--asc', type=int, help='ascent in 2048 units')
  parser.add_argument('--desc', type=int, help='descent in 2048 units')
  parser.add_argument('--wght', type=float, default=400)
  parser.add_argument('--opsz', type=float, default=14)
  parser.add_argument('--bind', help='carry the signature this .xttp patch '
                      'expects')
  args = parser.parse_args()

  font = load(args.source, args.wght, args.opsz)
  cmap = dict(font.getBestCmap())
  copyrights = [font['name'].getDebugName(0)]
  if args.latin:
    latin = load(args.latin, args.wght, args.opsz)
    cmap = merge_latin(font, latin)
    copyrights.append(latin['name'].getDebugName(0))
  names = [
      (0, ' '.join(c for c in copyrights if c)),
      (1, args.name),
      (13, 'This Font Software is licensed under the SIL Open Font License, '
           'Version 1.1.'),
      (14, 'https://openfontlicense.org'),
  ]
  signature = patch_binding(args.bind) if args.bind else bytes(0x100)
  data, glyphs, pages = build(font, cmap, names, args.asc, args.desc,
                              signature)
  with open(args.output, 'wb') as f:
    f.write(data)
  print(f'{args.output}: {len(data)} bytes, {glyphs} glyphs, {pages} pages, '
        f'{len(cmap)} codepoints')


if __name__ == '__main__':
  main()
