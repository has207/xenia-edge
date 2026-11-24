"""Z, guard band and user plane clipping with clipping enabled: Direct3D's
PA_CL_CLIP_CNTL (dx_clip_space_def) and PA_CL_VTE_CNTL (viewport transform, W
not reciprocal) and a 64x32 viewport (X scale 32 offset 32, Y scale -16 offset
16, Z scale 1 offset 0 unless a test changes them), into a D24S8 depth buffer
resolved raw like the depth suite.

Each test first fills the render target with a quad at NDC Z 0.5 writing
stencil 0x11, then draws its own with stencil 0x5A (depth ALWAYS with writes,
stencil REPLACE, no culling). A pixel's stencil says whether the test's draw
covered it and its depth what Z it got. Unless a test changes them, every draw
spans window X -2 to 66 and Y -2 to 34 (NDC X +-1.0625, Y +-1.125, inside the
guard band of 2) as two triangles or a rectangle list. NDC Z is a function of
window X only so every row should read the same:
- ramps: NDC Z = base + (x - x0) * slope, crossing a plane (Z = 0 or 1) 3/8
  pixel past a pixel center (integer window coordinates). A clipped edge and
  a clamped one then differ by whole columns even if the samples sit a
  quarter pixel off the centers. 'both ramp' is (x - 15.375) / 32 with Z 0 at
  15.375 and 1 at 47.375. 'inside' (0.5 + (x - 31.375) / 128) crosses neither
  plane. Variants: clip-space W 2; W 1 on the left and 4 on the right; slopes
  of 2^-20 per pixel around 0 and 1.
- constant Z with W 1 at and next to 0 and 1: what is inclusive and whether
  anything past a plane by less than some tolerance is kept.
- 'both ramp' with other viewport Z scales and offsets (narrowed, reversed and
  past [0, 1]): clipping in clip space or against the window depth range and
  what depth a fragment gets.
- OpenGL clip space (dx_clip_space_def 0): Z = -1 + (x - 15.375) / 16, at -1
  and 1 in the same columns as 'both ramp', with viewport Z scale 0.5 offset
  0.5 or scale 1 offset 0. Constant Z at and next to -1.
- Clipping disabled and D24FS8.
- PA_CL_VTE_CNTL with W given as 1/W (vtx_w0_fmt 0) or Z as Z/W (vtx_z_fmt 1)
  on 'both ramp' at W 2. With 1/W = float32(1/3): Z at and next to 3. Both
  formats on the OpenGL ramp, Z/W at W 1.5. In OpenGL clip space with
  1/W = 1/2, Z at and past -1/2.
- Z computed by the vertex shader as a product (r1.z *= r2.x) landing a
  fraction of an ulp past 1 (or -1 in OpenGL clip space) where rounding toward
  zero keeps it on the plane and rounding to nearest may push it past. Also a
  1 x 1 control and -2^-140, a denormal product past the near plane.
- Rectangle lists (the Resolve's three corners) at constant Z past either
  plane and on 'both ramp', some also with viewport Z scale 0.5 offset 0.25.
- The guard band (PA_CL_GB_HORZ_CLIP_ADJ and PA_CL_GB_VERT_CLIP_ADJ) at 2
  unless a test changes it. Narrowed on one axis, its edges sit 3/8 pixel
  outside columns 16 and 48 or rows 8 and 24 if it bounds the raw XY by the
  raw W. The other axis is at NDC +-4. Variants: NDC Z 0.5 with W 1 and 2;
  W 2 given as 1/W at NDC Z 0.125; XY given as XY/W (vtx_xy_fmt 1). With a
  viewport half as wide as the scissor: whether anything outside the viewport
  is drawn up to the guard band. A rectangle list under the narrowed guard
  band.
- The discard band (PA_CL_GB_HORZ_DISC_ADJ and PA_CL_GB_VERT_DISC_ADJ) at 1
  unless a test changes it, with viewport X scale 16 and the guard band at
  NDC +-4. Draws with every vertex outside the viewport: right of it and
  below a viewport Y scale -8, each with the horizontal and vertical
  adjustments at (1, 4) and (4, 1); on both sides; a rectangle list;
  clipping disabled; a point sprite centered past the band reaching into the
  viewport and a line past the band, at 1 and 4. W 2 given as 1/W inside the
  viewport, outside the band only if it bounds the raw X by the raw W, at 1
  and 4. The nearest vertices at and one ulp past the band.
- All 6 user clip planes enabled on 'both ramp' together with the Z planes
  and the guard band. Where they shouldn't cut, the user planes are W >= 0
  and the guard band is at NDC +-4. Variants: only the Z planes cutting; one
  user plane cutting rows 0 to 10 (each of the 6); the guard band cutting
  outside columns 24 to 40 or rows 8 to 24; all 6 user planes cutting
  (columns 19 to 44, rows 11 to 21 and two diagonal corners).
- Last since an unknown bit may hang the GPU: each PA_CL_CLIP_CNTL bit outside
  the user clip plane and clip space fields on 'both ramp' (bits 26 and 27
  disable near and far Z clipping on later AMD GPUs).
The pixel shader is the rb suite's over a zero texture, as in zdepth.
"""

import math
import os
import sys
from fractions import Fraction

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import depth  # noqa: E402
import gpugen  # noqa: E402
import pm4  # noqa: E402
import rb  # noqa: E402
import xenos as x  # noqa: E402
import zdepth  # noqa: E402

f2w, w2f = gpugen.f2w, gpugen.w2f
CLIP_CNTL = 1 << 19  # dx_clip_space_def
CLIP_DISABLE = 1 << 16
VTE_CNTL = 0x43F  # viewport scale and offset, vtx_w0_fmt (W not reciprocal)
VTE_W_RECIPROCAL = VTE_CNTL & ~(1 << 10)
VTE_Z_DIVIDED = VTE_CNTL | (1 << 9)  # vtx_z_fmt
VTE_XY_DIVIDED = VTE_CNTL | (1 << 8)  # vtx_xy_fmt
FILL_STENCIL = 0x11
LEFT, RIGHT, TOP, BOTTOM = -2.0, 66.0, -2.0, 34.0
SLOPE_FINE = 2.0 ** -20
GUARD_BAND = (2.0, 2.0)
# Guard band NDC bounds placing the edges at window X 15.625 and 48.375 or Y
# 7.625 and 24.375, and one where it doesn't cut.
NARROW_GUARD_BAND_X, NARROW_GUARD_BAND_Y = 131 / 256, 67 / 128
WIDE_GUARD_BAND = 4.0
NARROW_GUARD_BANDS = (('X', (NARROW_GUARD_BAND_X, WIDE_GUARD_BAND)),
                      ('Y', (WIDE_GUARD_BAND, NARROW_GUARD_BAND_Y)))
# PA_CL_CLIP_CNTL bits other than ucp_ena (0-5), ps_ucp_mode (14-15),
# clip_disable (16) and dx_clip_space_def (19).
CLIP_BITS = list(range(6, 14)) + [17, 18] + list(range(20, 32))
TRIANGLES = ((LEFT, TOP), (RIGHT, TOP), (LEFT, BOTTOM), (RIGHT, TOP),
             (RIGHT, BOTTOM), (LEFT, BOTTOM))
# A rectangle list's three vertices, as Direct3D's Resolve passes them.
RECTANGLE = ((LEFT, TOP), (RIGHT, TOP), (RIGHT, BOTTOM))


def f32(value):
    return w2f(f2w(value))


def ramp(x0, slope, base=0.0):
    return lambda xw: base + (xw - x0) * slope


def constant(z):
    return lambda xw: z


def vertices(position, factor=1.0, corners=TRIANGLES):
    """Two triangles (or a rectangle) over the viewport and past it,
    position(window X, Y), attribute X `factor`."""
    return [[f2w(v) for v in position(xw, yw)] +
            [f2w(factor), 0, 0, f2w(1.0)] for xw, yw in corners]


def quad(ndc_z, w=(1.0, 1.0), factor=1.0, w_reciprocal=False,
         z_divided=False, corners=TRIANGLES):
    """vertices() at NDC Z ndc_z(window X), clip-space W w[0] on the left and
    w[1] on the right, given as 1/W and Z/W if asked."""
    def position(xw, yw):
        vw = w[0] if xw < 32 else w[1]
        z = ndc_z(xw)
        return ((xw - 32) / 32 * vw, (16 - yw) / 16 * vw,
                z if z_divided else z * vw, 1 / vw if w_reciprocal else vw)
    return vertices(position, factor, corners)


def product(ulps, negative=False):
    """(a, b), float32s whose exact product is 1 + `ulps` ulp of 1 (near
    enough), or its negation: a = 1 + i ulp, b = 1 - j 2^-24, where
    a b - 1 = d 2^-24 - i j 2^-47 with d = 2i - j."""
    d = math.ceil(2 * ulps + 0.25)
    i = round(math.sqrt((d / 2 - ulps) * 2 ** 23))
    a = 1 + Fraction(i, 2 ** 23)
    b = 1 - Fraction(2 * i - d, 2 ** 24)
    assert f32(float(a)) == a and f32(float(b)) == b
    return (-float(a) if negative else float(a)), float(b)


def clip_state(clip_cntl=CLIP_CNTL, vte_cntl=VTE_CNTL, z=(1.0, 0.0),
               viewport_x=(32.0, 32.0), viewport_y=(-16.0, 16.0),
               guard_band=GUARD_BAND, guard_band_discard=(1.0, 1.0),
               user_clip_planes=None):
    """z, viewport_x and viewport_y are the viewport scales and offsets,
    guard_band and guard_band_discard the horizontal and vertical clip and
    discard adjustments, user_clip_planes the 6 planes' XYZW if they're
    written."""
    state = [('PA_CL_CLIP_CNTL', [clip_cntl]),
             ('PA_CL_VTE_CNTL', [vte_cntl]),
             ('PA_CL_VPORT_XSCALE',
              [f2w(v) for v in (viewport_x[0], viewport_x[1], viewport_y[0],
                                viewport_y[1], z[0], z[1])]),
             ('PA_CL_GB_HORZ_CLIP_ADJ', [f2w(guard_band[0])]),
             ('PA_CL_GB_VERT_CLIP_ADJ', [f2w(guard_band[1])]),
             ('PA_CL_GB_HORZ_DISC_ADJ', [f2w(guard_band_discard[0])]),
             ('PA_CL_GB_VERT_DISC_ADJ', [f2w(guard_band_discard[1])])]
    if user_clip_planes:
        state.append(('PA_CL_UCP_0_X',
                      [f2w(v) for plane in user_clip_planes for v in plane]))
    return state


def build(suite):
    words, draws = rb.RECT + [0, 0], {}

    def add(key, vertices, primitive=pm4.PRIM_TRIANGLE_LIST):
        assert key not in draws, key
        draws[key] = (primitive, len(words) // 8, len(vertices))
        words.extend(w for v in vertices for w in v)
        return key

    add('fill', quad(constant(0.5)))
    fill_registers = clip_state() + [
        ('RB_STENCILREFMASK', [(rb.STENCILREFMASK & ~0xFF) | FILL_STENCIL])]
    fetch = rb.fetch('zero')
    plain = gpugen.interpolating_vertex_shader(8)
    # r1.z *= r2.x before the exports.
    scaling = gpugen.interpolating_vertex_shader(8, [
        x.alu(vector_op=x.V_MUL, vector_dest=gpugen.INPUT_REGISTER,
              vector_mask=0x4, src1=x.r(gpugen.INPUT_REGISTER),
              src2=x.r(gpugen.INPUT_REGISTER + 1, 'xxxx'))])

    def test(name, key, fmt=depth.D24S8, shader=plain, registers=(), **state):
        """state goes to clip_state, registers are written after it."""
        after = clip_state(**state) + list(registers) + [
            ('RB_STENCILREFMASK', [rb.STENCILREFMASK])]
        rb.test(suite, name, 'zero', rb.registers(depth_format=fmt),
                depth.RESOLVE, draw=draws[key],
                passes=[(fill_registers, fetch, draws['fill'])],
                after_passes=after, shader=shader,
                program_cntl=zdepth.PROGRAM_CNTL)

    both = add('both ramp', quad(ramp(15.375, 1 / 32)))
    test('inside', add('inside', quad(ramp(31.375, 1 / 128, 0.5))))
    test('near ramp', add('near ramp', quad(ramp(31.375, 1 / 64))))
    test('far ramp', add('far ramp', quad(ramp(31.375, 1 / 64, 1.0))))
    test('both ramp', both)
    test('both ramp W 2',
         add('both ramp W 2', quad(ramp(15.375, 1 / 32), (2.0, 2.0))))
    test('both ramp W 1 to 4',
         add('both ramp W 1 to 4', quad(ramp(15.375, 1 / 32), (1.0, 4.0))))
    test('fine near ramp',
         add('fine near ramp', quad(ramp(31.375, SLOPE_FINE))))
    test('fine far ramp',
         add('fine far ramp', quad(ramp(31.375, SLOPE_FINE, 1.0))))

    for name, z in (('0', 0.0), ('-0', -0.0), ('-2^-149', -2.0 ** -149),
                    ('-2^-126', -2.0 ** -126), ('-2^-24', -2.0 ** -24),
                    ('-2^-20', -2.0 ** -20), ('-2^-16', -2.0 ** -16),
                    ('-2^-12', -2.0 ** -12), ('-2^-8', -2.0 ** -8),
                    ('1-2^-24', 1 - 2.0 ** -24), ('1', 1.0),
                    ('1+2^-23', 1 + 2.0 ** -23), ('1+2^-20', 1 + 2.0 ** -20),
                    ('1+2^-16', 1 + 2.0 ** -16), ('1+2^-12', 1 + 2.0 ** -12),
                    ('1+2^-8', 1 + 2.0 ** -8)):
        key = f'Z {name}'
        test(key, add(key, quad(constant(z))))

    for z in ((0.5, 0.25), (-1.0, 1.0), (2.0, -0.5), (1.0, 0.5)):
        test(f'both ramp viewport Z scale {z[0]:g} offset {z[1]:g}', both,
             z=z)

    gl_ramp = ramp(15.375, 1 / 16, -1.0)
    gl = add('OpenGL both ramp', quad(gl_ramp))
    test('OpenGL both ramp', gl, clip_cntl=0, z=(0.5, 0.5))
    test('OpenGL both ramp viewport Z scale 1 offset 0', gl, clip_cntl=0)
    for name, z in (('-1', -1.0), ('-1-2^-23', -1 - 2.0 ** -23)):
        key = f'OpenGL Z {name}'
        test(key, add(key, quad(constant(z))), clip_cntl=0, z=(0.5, 0.5))

    test('both ramp D24FS8', both, fmt=depth.D24FS8)
    test('no clip both ramp', both, clip_cntl=CLIP_CNTL | CLIP_DISABLE)
    test('no clip both ramp D24FS8', both, clip_cntl=CLIP_CNTL | CLIP_DISABLE,
         fmt=depth.D24FS8)

    key = 'both ramp W 2 as 1/W'
    test(key, add(key, quad(ramp(15.375, 1 / 32), (2.0, 2.0),
                            w_reciprocal=True)),
         vte_cntl=VTE_W_RECIPROCAL)
    key = 'both ramp W 2 Z as Z/W'
    test(key, add(key, quad(ramp(15.375, 1 / 32), (2.0, 2.0),
                            z_divided=True)),
         vte_cntl=VTE_Z_DIVIDED)
    # 1/W = float32(1/3), clip-space X and Y for W 3, Z at and next to 3.
    for name, z in (('3-2^-21', 3 - 2.0 ** -21), ('3-2^-22', 3 - 2.0 ** -22),
                    ('3', 3.0), ('3+2^-22', 3 + 2.0 ** -22),
                    ('3+2^-21', 3 + 2.0 ** -21)):
        key = f'1/W 1/3 Z {name}'
        test(key, add(key, vertices(
                 lambda xw, yw, z=z: ((xw - 32) / 32 * 3, (16 - yw) / 16 * 3,
                                      z, f32(1 / 3)))),
             vte_cntl=VTE_W_RECIPROCAL)

    for name, (a, b), clip_cntl, z in (
            ('product Z 1 x 1', (1.0, 1.0), CLIP_CNTL, (1.0, 0.0)),
            ('product Z 1+0.25ulp', product(0.25), CLIP_CNTL, (1.0, 0.0)),
            ('product Z 1+0.75ulp', product(0.75), CLIP_CNTL, (1.0, 0.0)),
            ('product Z 1+1.25ulp', product(1.25), CLIP_CNTL, (1.0, 0.0)),
            ('product Z -2^-140', (-2.0 ** -70, 2.0 ** -70), CLIP_CNTL,
             (1.0, 0.0)),
            ('OpenGL product Z -1-0.75ulp', product(0.75, True), 0,
             (0.5, 0.5)),
            ('OpenGL product Z -1-1.25ulp', product(1.25, True), 0,
             (0.5, 0.5))):
        test(name, add(name, quad(constant(a), factor=b)),
             clip_cntl=clip_cntl, z=z, shader=scaling)

    for name, ndc_z in (('Z -1', constant(-1.0)),
                        ('Z -2^-8', constant(-2.0 ** -8)),
                        ('Z 1+2^-23', constant(1 + 2.0 ** -23)),
                        ('Z 2', constant(2.0)),
                        ('both ramp', ramp(15.375, 1 / 32))):
        key = f'rectangle {name}'
        test(key, add(key, quad(ndc_z, corners=RECTANGLE),
                      pm4.PRIM_RECTANGLE_LIST))
    for name in ('Z -1', 'Z 2', 'both ramp'):
        test(f'rectangle {name} viewport Z scale 0.5 offset 0.25',
             f'rectangle {name}', z=(0.5, 0.25))

    key = 'OpenGL both ramp W 2 as 1/W'
    test(key, add(key, quad(gl_ramp, (2.0, 2.0), w_reciprocal=True)),
         clip_cntl=0, vte_cntl=VTE_W_RECIPROCAL, z=(0.5, 0.5))
    # Z at -W and W in window X 7.375 and 55.375.
    key = 'OpenGL both ramp W 1.5 Z as Z/W'
    test(key, add(key, quad(gl_ramp, (1.5, 1.5), z_divided=True)),
         clip_cntl=0, vte_cntl=VTE_Z_DIVIDED, z=(0.5, 0.5))
    # 1/W = 1/2, clip-space X and Y for W 2, Z at and past -1/2 (the near
    # plane if the clipper takes the raw W).
    for name, z in (('-1/2', -0.5), ('-1/2-2^-24', -0.5 - 2.0 ** -24)):
        key = f'OpenGL 1/W 1/2 Z {name}'
        test(key, add(key, vertices(
                 lambda xw, yw, z=z: ((xw - 32) / 32 * 2, (16 - yw) / 16 * 2,
                                      z, 0.5))),
             clip_cntl=0, vte_cntl=VTE_W_RECIPROCAL, z=(0.5, 0.5))

    # The raw XY over the raw W is `ratio` times the NDC XY, the guard band
    # for NDC bounds b being b * ratio.
    for suffix, vertex_data, ratio, vte_cntl in (
            ('', quad(constant(0.5)), 1.0, VTE_CNTL),
            (' W 2', quad(constant(0.5), (2.0, 2.0)), 1.0, VTE_CNTL),
            (' W 2 as 1/W',
             quad(constant(0.125), (2.0, 2.0), w_reciprocal=True), 4.0,
             VTE_W_RECIPROCAL),
            (' W 2 XY as XY/W', vertices(
                lambda xw, yw: ((xw - 32) / 32, (16 - yw) / 16, 1.0, 2.0)),
             0.5, VTE_XY_DIVIDED)):
        key = add(f'guard band{suffix}', vertex_data)
        for name, bounds in NARROW_GUARD_BANDS:
            test(f'guard band {name}{suffix}', key, vte_cntl=vte_cntl,
                 guard_band=tuple(b * ratio for b in bounds))
    wide = (WIDE_GUARD_BAND, WIDE_GUARD_BAND)
    # Viewport X scale 16 (window X 16 to 48) under the full scissor, the draw
    # spanning NDC X +-2.125: the guard band edges at window X 7.625 and
    # 56.375, or past the render target.
    def viewport_x_16(xw, yw):
        """Window X and Y under viewport X scale 16 at NDC Z 0.5."""
        return ((xw - 32) / 16, (16 - yw) / 16, 0.5, 1.0)
    key = add('guard band viewport X scale 16', vertices(viewport_x_16))
    for name, guard_band in (('X', (195 / 128, WIDE_GUARD_BAND)),
                             ('4', wide)):
        test(f'guard band {name} viewport X scale 16', key,
             guard_band=guard_band, viewport_x=(16.0, 32.0))
    key = add('rectangle guard band', quad(constant(0.5), corners=RECTANGLE),
              pm4.PRIM_RECTANGLE_LIST)
    for name, guard_band in NARROW_GUARD_BANDS:
        test(f'rectangle guard band {name}', key, guard_band=guard_band)

    def discard_test(name, window_x, window_y=(TOP, BOTTOM),
                     w_reciprocal=False, rectangle=False, **state):
        """A draw spanning window_x and window_y (two triangles or a
        rectangle list) under viewport X scale 16 (window X 16 to 48), the
        guard band at NDC +-4 and discard 1 unless state changes them, at NDC
        Z 0.5 (0.125 with W 2 given as 1/W)."""
        state.setdefault('viewport_x', (16.0, 32.0))
        state.setdefault('viewport_y', (-16.0, 16.0))
        state.setdefault('guard_band', wide)
        if w_reciprocal:
            state.setdefault('vte_cntl', VTE_W_RECIPROCAL)
        (x_scale, x_offset), (y_scale, y_offset) = (state['viewport_x'],
                                                    state['viewport_y'])
        x0, x1 = window_x
        y0, y1 = window_y

        def position(cx, cy):
            xw = x0 + (cx - LEFT) * (x1 - x0) / (RIGHT - LEFT)
            yw = y0 + (cy - TOP) * (y1 - y0) / (BOTTOM - TOP)
            ndc = ((xw - x_offset) / x_scale, (yw - y_offset) / y_scale)
            if w_reciprocal:
                return (ndc[0] * 2, ndc[1] * 2, 0.25, 0.5)
            return ndc + (0.5, 1.0)
        key = add(f'discard {name}', vertices(
            position, corners=RECTANGLE if rectangle else TRIANGLES),
            pm4.PRIM_RECTANGLE_LIST if rectangle else pm4.PRIM_TRIANGLE_LIST)
        test(f'discard {name}', key, **state)

    # Right of the viewport, window X 50 to 66 (NDC X 1.125 to 2.125). Below
    # viewport Y scale -8 offset 16 (window Y 8 to 24) over the whole width,
    # window Y 26 to 34 (NDC Y -1.25 to -2.25). The other axis is on both
    # sides of the band.
    right = (50.0, 66.0)
    for discard in ((1.0, 4.0), (4.0, 1.0)):
        adjustments = f'X {discard[0]:g} Y {discard[1]:g}'
        discard_test(f'{adjustments} right of viewport', right,
                     guard_band_discard=discard)
        discard_test(f'{adjustments} below viewport Y scale -8', (LEFT, RIGHT),
                     (26.0, 34.0), viewport_x=(32.0, 32.0),
                     viewport_y=(-8.0, 16.0), guard_band_discard=discard)
    # Every vertex outside the band, on both sides of it.
    discard_test('both sides', (4.0, 60.0))
    # W 2 given as 1/W with the viewport over the render target: NDC X 0.5 to
    # 0.75 (window X 48 to 56), outside the band at 1 if it bounds the raw X
    # (2 * NDC X) by the raw W (1/2).
    for discard in (1.0, 4.0):
        discard_test(f'{discard:g} at W 2 as 1/W', (48.0, 56.0),
                     w_reciprocal=True, viewport_x=(32.0, 32.0),
                     guard_band_discard=(discard, discard))
    discard_test('rectangle right of viewport', right, rectangle=True)
    discard_test('no clip right of viewport', right,
                 clip_cntl=CLIP_CNTL | CLIP_DISABLE)
    # The nearest vertices at NDC X 1 and one ulp past it.
    discard_test('edge at 1', (48.0, 64.0))
    discard_test('edge past 1', (48.0 + 16 * 2.0 ** -23, 64.0))
    # A point centered at window X 50.375 (NDC X 1.1484375), Y 16.375, with
    # PA_SU_POINT_SIZE 0x80 a half size of 8 (12.4 fixed point): X 42.375 to
    # 58.375, reaching into the viewport. A line from window X 50 to 62 at Y
    # 16, 1 pixel wide with PA_SU_LINE_CNTL 8 from the initial state, the ends
    # at pixel centers where the diamond exit rule and the host's agree.
    for name, primitive, corners, registers in (
            ('point', pm4.PRIM_POINT_LIST, ((50.375, 16.375),),
             [('PA_SU_POINT_SIZE', [0x00800080])]),
            ('line', pm4.PRIM_LINE_LIST, ((50.0, 16.0), (62.0, 16.0)), [])):
        key = add(f'discard {name}', vertices(viewport_x_16, corners=corners),
                  primitive)
        for discard in (1.0, 4.0):
            test(f'discard {discard:g} {name} right of viewport', key,
                 viewport_x=(16.0, 32.0), guard_band=wide,
                 guard_band_discard=(discard, discard), registers=registers)

    # Clip-space planes kept where X Y Z W . plane >= 0, with W 1: X =
    # (window X - 32) / 32, Y = (16 - window Y) / 16.
    clip_all = CLIP_CNTL | 0x3F
    uncut = (0.0, 0.0, 0.0, 1.0)
    from_row_11 = (0.0, -1.0, 0.0, 43 / 128)  # Window Y >= 10.625.
    test('all planes', both, clip_cntl=clip_all, guard_band=wide,
         user_clip_planes=[uncut] * 6)
    for i in range(6):
        test(f'all planes user plane {i}', both, clip_cntl=clip_all,
             guard_band=wide,
             user_clip_planes=[from_row_11 if j == i else uncut
                               for j in range(6)])
    # Edges at window X 23.625 and 40.375 or Y 7.625 and 24.375.
    for name, guard_band in (('X', (67 / 256, WIDE_GUARD_BAND)),
                             ('Y', (WIDE_GUARD_BAND, NARROW_GUARD_BAND_Y))):
        test(f'all planes guard band {name}', both, clip_cntl=clip_all,
             guard_band=guard_band, user_clip_planes=[uncut] * 6)
    test('all planes 6 user planes cutting', both, clip_cntl=clip_all,
         guard_band=wide, user_clip_planes=[
             (1.0, 0.0, 0.0, 107 / 256),  # Window X >= 18.625.
             (-1.0, 0.0, 0.0, 99 / 256),  # Window X <= 44.375.
             from_row_11,
             (0.0, 1.0, 0.0, 43 / 128),  # Window Y <= 21.375.
             (2.0, -1.0, 0.0, 117 / 128),  # Window X + Y >= 33.375.
             (-2.0, 1.0, 0.0, 109 / 128)])  # Window X + Y <= 61.625.

    for bit in CLIP_BITS:
        test(f'both ramp clip bit {bit}', both,
             clip_cntl=CLIP_CNTL | (1 << bit))

    suite.set_inputs(words, 8)
    suite.add_texture('zero', rb.fill((0.0, 0.0, 0.0, 0.0)))
