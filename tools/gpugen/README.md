# gpugen

Records what the Xenos GPU computes on a console and checks Xenia against it,
the way `tools/xamgen` does for xam exports.

A suite in `suites/` lists tests. `gpugen.py build` turns it into one XEX,
built with `tools/xex`, with a fixed executor that brings up the command
processor through the kernel the way Direct3D does and submits the suite's
PM4 stream one test at a time. A test is a draw whose shaders, written with
the microcode encoder in `xenos.py`, store their results with memory export,
or a draw into EDRAM followed by a Resolve. `run.py` runs that XEX on an RGH
console over XBDM (XeCLI `rgh.exe`) or in the Release build of Xenia, and
compares runs.

```
python3 tools/gpugen/gpugen.py build tools/gpugen/suites/smoke.py OUT \
    --microcode IMAGE
python3 tools/gpugen/run.py console OUT
python3 tools/gpugen/run.py xenia OUT
python3 tools/gpugen/run.py xenia OUT \
    --xenia-arg=--render_target_path=accuracy
python3 tools/gpugen/run.py diff OUT/console_results.json \
    OUT/xenia_results.json
```

A console run needs the command processor microcode Direct3D passes to
`VdInitializeEngines`: the kernel's own doesn't run these streams. It isn't
distributed; `--microcode` takes the decrypted, decompressed image of an XEX
that links Direct3D and finds it there by SHA-1. Without it the build only
runs in Xenia. Like xamgen's, `run.py xenia` runs the Release build or the one
`XENIA_EXE` names, `run.py console` needs XeCLI's `rgh` on `PATH` or named by
`RGH`, and a console run starts from Aurora.

Each test is one segment ending in a fence, so a hang names its test. A GPU
left busy keeps the title from exiting; `rgh reboot --yes` recovers. Things
the console needs that Xenia doesn't:

- Float constants go out in whole groups of four aligned registers, never
  across the vertex and pixel banks; fetch constants as whole 6-dword slots.
- A fetch reading the result of an earlier fetch in the same clause needs the
  serialize bit, and a mini vertex fetch must stay within the 8 dwords the
  preceding full fetch prefetches (`fetch_inputs`).
- Vertex shader memory export runs in multipass export mode and ends like
  Direct3D's memexport shaders (`memexport_shader`).
- PsParamGen.xy is the pixel position only with
  SQ_CONTEXT_MISC.sc_output_screen_xy, and multisampling takes
  PA_SC_AA_CONFIG as well as RB_SURFACE_INFO.msaa_samples.

Suites:

- `cp`, `smoke`: bring-up checks.
- `alu`, `specials`, `ops2`, `dot`, `dotprobe`, `transcendental`: shader ALU
  results, specials and rounding. `a0`: the address register. `sweep`: every
  mantissa of one op over a binade, or sin/cos over a linear range, chosen
  with `GPUGEN_SWEEP` (`GPUGEN_VERTICES` shortens it); its results are raw
  `.bin` files.
- `vfetch`, `edges`, `mexport`: vertex fetch conversions and indexing, memory
  export formats and addressing.
- `tfetch`, `tfetch_formats`, `tfilter`, `tfilter_probes`: texture fetch
  conversions, block formats, endianness, filtering and addressing.
- `getbcf`: getBCF in each clamp mode, filter, format and dimension, next to
  a tfetch of the border at the same coordinates.
- `pixel`, `pixel_probes`, `snap`, `msaa`: pixel shader inputs (interpolation,
  PsParamGen), vertex snapping and multisample coverage.
- `rb`, `blend`, `depth`, `zdepth`, `alpha`, `msaarb`: render target formats,
  blending, depth formats and polygon offset, alpha test and alpha to mask,
  MSAA samples and resolves.
- `blend_products`: blending on the fixed point formats one product at a time,
  each operand swept or held.
- `clip`: near and far Z clipping across viewport Z ranges, both clip spaces,
  the PA_CL_CLIP_CNTL bits, the PA_CL_VTE_CNTL formats and rectangle lists.
  Guard band, discard band and user clip plane clipping.
