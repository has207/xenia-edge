# xamgen

Records how xam and kernel exports behave on a console and checks Xenia
against it, the way `src/xenia/cpu/ppc/testing/instr__gen_*.s` do for
instructions.

A corpus in `corpus/` lists scenarios of export calls. `xamgen.py build` turns
it into one XEX, built with `tools/xex`, with a fixed executor that makes the
calls and records r3, the memory each call writes and overlapped completions.
`run.py` runs that XEX on an RGH console over XBDM (XeCLI `rgh.exe`) or in the
Release build of Xenia, and compares runs. The expected results checked in under
`src/xenia/kernel/xam/testing/xam__gen_<corpus>.json`, or
`src/xenia/kernel/xboxkrnl/testing/xboxkrnl__gen_<corpus>.json` for a corpus of
kernel exports, come from a console.

Where an export takes a callback, a step passes a recorder, which logs the
arguments it's called with into the arena for a capture. A step marked `void`
calls an export that returns nothing, so its r3 isn't compared.

A step can also call a routine, guest code the corpus writes with
`tools/xex/ppcasm.py`. A routine can declare functions with .pdata entries,
which go in the image's exception directory, so the kernel finds them and their
exception handlers. A corpus whose steps fault on purpose sets
`corpus.unwatched`: XBDM with a notification session open keeps raising a
first-chance exception instead of dispatching it, so `run.py console` runs it
without one and polls the executor's done word instead.

```
python3 tools/xamgen/xamgen.py build tools/xamgen/corpus/content.py OUT
python3 tools/xamgen/run.py console OUT
python3 tools/xamgen/run.py expect OUT/console_results.json \
    src/xenia/kernel/xam/testing/xam__gen_content.json --source "..."
python3 tools/xamgen/run.py xenia OUT
python3 tools/xamgen/run.py check \
    src/xenia/kernel/xam/testing/xam__gen_content.json OUT/xenia_results.json
```

`run.py xenia` runs the host's Release build from `build/bin` (or the Windows
one from WSL), or the build `XENIA_EXE` names, and the executor leaves its
results beside the XEX. `run.py console` needs XeCLI's `rgh`, on `PATH` or
named by `RGH`.
Both sides need a profile signed in at slot 0. A console run only starts from
Aurora and goes back to it. A call that faults on a console is marked
`faults=True` in its corpus, which records it without running it. A scenario
that would damage the host on a Xenia without the fix it tests is made with
`console_only=<reason>`. `run.py xenia` refuses a build that holds one. Leave it
out with `--only` until the fix is in.
