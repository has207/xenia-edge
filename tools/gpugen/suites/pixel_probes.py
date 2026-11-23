"""What the pixel suite left open about interpolation and PsParamGen, with
its shaders and output layout. Triangle attributes are one-hot by the vertex's
place in the draw, (1, 0, 0, w), (0, 1, 0, w), (0, 0, 1, w), unless a test
says otherwise; lines and rectangles take the pixel suite's (X, Y, X + Y, 1)
of the position.

- base: one triangle per 8x8 cell, BASE_SHAPES as given, reversed, and the
  first 20 rotated once (w = cell), to pin down which vertex I and J are
  measured from. 'base scaled' / 'base scaled shifted': SHAPES[1] of the pixel
  suite at 2x from (16, 16), and moved by half a pixel.
- translate: the pixel suite's 'triangle' moved by whole pixels, to show
  whether I and J depend on where it is.
- right a b: (-0.5, -0.5), (a - 0.5, -0.5), (-0.5, b - 0.5), exact I and J
  (px + 0.5) / a and (py + 0.5) / b.
- mirrored: 'triangle' mirrored across x = y. large: one triangle across the
  window, (-0.5, -0.5), (63.5, 2.5), (1.5, 62.5).
- attr order / attr delta: 'triangle' with X = ORDER_X / DELTA_X; zero sign:
  the quad's first triangle with X = (0, 1, -1), all -0, (-1, 1, 1).
- lines: x-major length 13, reversed, y-major, 45 degrees, widths 2 and 3.
- rectangles: 13x11, the corner not first, reversed winding.
- sprites: 3x3, 5x5 and 2x6 (PA_SU_POINT_SIZE halves) at (8a + 3.25,
  8b + 3.75).
- no screen xy: a point on every pixel of the window, indexed by attribute,
  without sc_output_screen_xy; and with PA_SC_WINDOW_OFFSET (16, 8).
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gpugen  # noqa: E402
import pixel  # noqa: E402
import pm4  # noqa: E402

BASE_SHAPES = (
    [((0.25, 0.25), (0.25, 7.25), (7.25, 7.25)),
     ((3.75, 0.25), (7.25, 3.75), (0.25, 3.75)),
     ((7.25, 3.75), (3.75, 7.25), (3.75, 0.25)),
     ((7.25, 0.25), (7.25, 7.25), (0.25, 7.0)),
     ((7.25, 0.25), (7.25, 7.25), (0.25, 7.5))] +
    [((7.25, 0.25), (7.25, 7.25), (0.25, 0.25 + 0.5 * k)) for k in range(15)] +
    [((0.25, 0.25), (2.25, 3.75), (0.25, 7.25)),
     ((7.25, 0.25), (0.25, 3.25), (5.25, 7.25))])
# X attributes found by search: under ORDER_X the candidate accumulation
# orders, under DELTA_X truncated and rounded deltas, differ at most of
# 'triangle's pixels.
ORDER_X = (0xC07FE894, 0xC09BB990, 0xBF157DE4)
DELTA_X = (0x4B800000, 0x3FC00000, 0xBFA00000)
QUAD_FIRST = tuple(pixel.QUAD[:3])
RIGHT = [(3, 3), (5, 5), (7, 7), (11, 11), (13, 13), (15, 15), (17, 3),
         (21, 13), (31, 29), (63, 61)]


def one_hot(corners, w=0.0, x_words=None):
    vertices = []
    for k, p in enumerate(corners):
        attribute = [0.0, 0.0, 0.0, w]
        attribute[k] = 1.0
        if x_words:
            attribute[0] = gpugen.w2f(x_words[k])
        vertices.append(pixel.vertex(p, attribute))
    return vertices


def moved(corners, dx, dy, scale=1):
    return tuple((scale * x + dx, scale * y + dy) for x, y in corners)


def base_cells():
    orders = [s for s in BASE_SHAPES] + [s[::-1] for s in BASE_SHAPES]
    orders += [s[1:] + s[:1] for s in BASE_SHAPES[:20]]
    vertices = []
    for cell, corners in enumerate(orders[:64]):
        vertices += one_hot(moved(corners, 8 * (cell % 8), 8 * (cell // 8)),
                            float(cell))
    return vertices


def point_grid():
    return [pixel.vertex((i % pixel.WINDOW, i // pixel.WINDOW),
                         (i, i % pixel.WINDOW, i // pixel.WINDOW, 7.0))
            for i in range(pixel.WINDOW * pixel.WINDOW)]


TRIANGLES = pm4.PRIM_TRIANGLE_LIST
BLOCKS = [('base', TRIANGLES, base_cells()),
          ('base scaled', TRIANGLES,
           one_hot(moved(pixel.SHAPES[1], 16, 16, 2))),
          ('base scaled shifted', TRIANGLES,
           one_hot(moved(pixel.SHAPES[1], 16.5, 16.5, 2)))]
BLOCKS += [(f'translate {dx} {dy}', TRIANGLES,
            one_hot(moved(pixel.TRIANGLE, dx, dy)))
           for dx, dy in ((0, 0), (24, 0), (0, 24), (24, 24), (8, 40))]
BLOCKS += [(f'right {a} {b}', TRIANGLES,
            one_hot(((-0.5, -0.5), (a - 0.5, -0.5), (-0.5, b - 0.5))))
           for a, b in RIGHT]
BLOCKS += [('mirrored', TRIANGLES,
            one_hot(tuple((y, x) for x, y in pixel.TRIANGLE))),
           ('large', TRIANGLES,
            one_hot(((-0.5, -0.5), (63.5, 2.5), (1.5, 62.5)))),
           ('attr order', TRIANGLES,
            one_hot(pixel.TRIANGLE, x_words=ORDER_X)),
           ('attr delta', TRIANGLES,
            one_hot(pixel.TRIANGLE, x_words=DELTA_X)),
           ('zero sign 0 1 -1', TRIANGLES,
            one_hot(QUAD_FIRST, x_words=(0x00000000, 0x3F800000, 0xBF800000))),
           ('zero sign -0', TRIANGLES,
            one_hot(QUAD_FIRST, x_words=(0x80000000,) * 3)),
           ('zero sign -1 1 1', TRIANGLES,
            one_hot(QUAD_FIRST, x_words=(0xBF800000, 0x3F800000, 0x3F800000)))]
LINES = [('line 13', ((-0.5, 7.25), (12.5, 8.75))),
         ('line reversed', ((15.5, 8.75), (-0.5, 7.25))),
         ('line y-major', ((7.25, -0.5), (8.75, 15.5))),
         ('line 45', ((-0.5, -0.5), (15.5, 15.5))),
         ('line wide', ((-0.5, 7.25), (15.5, 8.75)))]
BLOCKS += [(name, pm4.PRIM_LINE_LIST, pixel.positional(points))
           for name, points in LINES]
BLOCKS += [('rectangle 13x11', pm4.PRIM_RECTANGLE_LIST,
            pixel.positional(((-0.5, -0.5), (12.5, -0.5), (-0.5, 10.5)))),
           ('rectangle corner not first', pm4.PRIM_RECTANGLE_LIST,
            pixel.positional(((12.5, -0.5), (-0.5, -0.5), (12.5, 10.5)))),
           ('rectangle reversed', pm4.PRIM_RECTANGLE_LIST,
            pixel.positional(((-0.5, -0.5), (-0.5, 10.5), (12.5, -0.5)))),
           ('sprites', pm4.PRIM_POINT_LIST,
            [pixel.vertex((8 * a + 3.25, 8 * b + 3.75), (a, b, 0.0, 7.0))
             for b in range(8) for a in range(8)]),
           ('point grid', pm4.PRIM_POINT_LIST, point_grid())]


def build(suite):
    draws = pixel.set_blocks(suite, BLOCKS)
    probe = pixel.Probe()

    def test(name, block, registers=(), pixel_shader=None):
        probe.test(suite, name, draws[block], registers, pixel_shader)

    for name, _, _ in BLOCKS:
        if name in ('sprites', 'point grid', 'line wide'):
            continue
        test(name, name)
    # Nothing resets PA_SU_LINE_CNTL: no line test may follow.
    for width, half in ((2, 0x10), (3, 0x18)):
        test(f'line width {width}', 'line wide', [('PA_SU_LINE_CNTL', [half])])
    for name, size in (('3x3', 0x00180018), ('5x5', 0x00280028),
                       ('2x6', 0x00100030)):
        test(f'sprites {name}', 'sprites', [('PA_SU_POINT_SIZE', [size])])
    test('no screen xy grid', 'point grid', pixel.NO_SCREEN_XY,
         probe.by_attribute)
    test('no screen xy window offset', 'point grid',
         pixel.NO_SCREEN_XY + [('PA_SC_WINDOW_OFFSET', [(8 << 16) | 16])],
         probe.by_attribute)
