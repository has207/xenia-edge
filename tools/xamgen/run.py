#!/usr/bin/env python3
"""Runs a built xamgen corpus in Xenia or on a console and compares runs.

    python3 tools/xamgen/run.py xenia OUTDIR
    python3 tools/xamgen/run.py console OUTDIR
    python3 tools/xamgen/run.py diff A.json B.json
    python3 tools/xamgen/run.py expect RESULTS.json EXPECTED.json --source TEXT
    python3 tools/xamgen/run.py check EXPECTED.json RESULTS.json

xenia starts the host's Release build (or the Windows one from WSL, XENIA_EXE
names another) on OUTDIR/xamgen.xex with XENIA_ARGS, waits for the results file
the executor writes beside it and its DbgPrint line in the log, then kills it
(Xenia doesn't quit when a title it was started on exits). console uploads the
XEX to Hdd:\\oracle\\ over XBDM with XeCLI's rgh (RGH or on PATH), launches it,
watches XBDM's debug output until the executor unloads or faults and fetches
the results file. On a fault it reads the executor's progress word to name the
step and launches Aurora again. An unwatched corpus runs with no notification
session: console polls the executor's done word and writes its exit word, and
names the step the same way if it isn't done in time. Both decode the results
to OUTDIR/<where>_results.json.

Comparisons normalize first: the XUID and gamertag the corpus's identity
scenario reads become <X0> and <NAME0>, and a wait only records whether the
XOVERLAPPED completed. r3 is compared by its low 32 bits when the high half
only extends them. expect writes a run's normalized steps as the expected
results, check compares a run against them and exits nonzero on a mismatch.
"""

import argparse
import functools
import json
import os
import platform
import plistlib
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xamgen  # noqa: E402


WSL = sys.platform == 'linux' and 'microsoft' in platform.uname().release.lower()


def default_xenia():
    release = os.path.join(xamgen.REPO, 'build', 'bin')
    if sys.platform == 'win32' or WSL:
        return os.path.join(release, 'Windows', 'Release', 'xenia_edge.exe')
    if sys.platform == 'darwin':
        return os.path.join(release, 'macOS', 'Release', 'Xenia-edge.app')
    return os.path.join(release, 'Linux', 'Release', 'xenia_edge')


def bundle_binary(path):
    """The program a macOS .app bundle runs, or `path` if it isn't one."""
    plist = os.path.join(path, 'Contents', 'Info.plist')
    if not os.path.isfile(plist):
        return path
    with open(plist, 'rb') as f:
        name = plistlib.load(f)['CFBundleExecutable']
    return os.path.join(path, 'Contents', 'MacOS', name)


XENIA = bundle_binary(os.environ.get('XENIA_EXE') or default_xenia())
CONSOLE_DIR = 'Hdd:\\oracle'
AURORA = 'Hdd:\\Aurora\\Aurora.xex'
# Settings a user's config may change that results depend on, at what a
# console does. Without cache: the executor writes its results to game:, which
# is OUTDIR once Xenia may write there.
XENIA_ARGS = ['--license_mask=0', '--mount_cache=false',
              '--allow_game_relative_writes=true']


def from_wsl(program):
    """Whether `program` is a Windows one started from WSL, which takes Windows
    paths and gets a PID from WSL that can't stop it."""
    return WSL and program.lower().endswith('.exe')


def host_path(path, program):
    path = os.path.abspath(path)
    if not from_wsl(program):
        return path
    return subprocess.run(['wslpath', '-w', path], capture_output=True,
                          text=True, check=True).stdout.strip()


@functools.cache
def windows_folder(variable):
    """A Windows environment folder such as %LOCALAPPDATA% as a local path."""
    if not WSL:
        return os.environ.get(variable, '')
    value = subprocess.run(['cmd.exe', '/c', f'echo %{variable}%'],
                           capture_output=True, text=True).stdout.strip()
    return subprocess.run(['wslpath', '-u', value], capture_output=True,
                          text=True).stdout.strip()


@functools.cache
def rgh_exe():
    """XeCLI's rgh: RGH, else on PATH, else where its Windows installer puts
    it."""
    found = (os.environ.get('RGH') or shutil.which('rgh') or
             shutil.which('rgh.exe'))
    if not found and (sys.platform == 'win32' or WSL):
        found = os.path.join(windows_folder('LOCALAPPDATA'), 'Programs',
                             'XeCLI', 'rgh.exe')
    if not found or not os.path.exists(found):
        sys.exit('console: XeCLI rgh not found, set RGH to it')
    return found


def rgh(*args, timeout=60):
    result = subprocess.run([rgh_exe(), *args], capture_output=True, text=True,
                            timeout=timeout)
    return result.returncode, result.stdout + result.stderr


def windows_pids(program):
    """Windows PIDs of running processes of `program`."""
    out = subprocess.run(
        ['tasklist.exe', '/FO', 'CSV', '/NH', '/FI',
         f'IMAGENAME eq {os.path.basename(program)}'],
        capture_output=True, text=True).stdout
    return {int(line.split('","')[1]) for line in out.splitlines()
            if line.startswith('"')}


def crash_dumps(started):
    """Windows crash dumps of Xenia written since `started`."""
    if sys.platform != 'win32' and not WSL:
        return None, []
    folder = os.path.join(windows_folder('LOCALAPPDATA'), 'CrashDumps')
    if not os.path.isdir(folder):
        return folder, []
    return folder, [d for d in os.listdir(folder) if d.lower().startswith(
        'xenia_edge') and os.path.getmtime(os.path.join(folder, d)) >= started]


def decode_into(outdir, where, results_path):
    with open(os.path.join(outdir, 'xamgen.json')) as f:
        meta = json.load(f)
    with open(results_path, 'rb') as f:
        decoded = xamgen.decode(meta, f.read())
    out = os.path.join(outdir, f'{where}_results.json')
    with open(out, 'w') as f:
        json.dump(decoded, f, indent=1)
    print(f'{where}: {decoded["steps_done"]} steps done -> {out}')
    return decoded


def run_xenia(outdir, timeout):
    outdir = os.path.abspath(outdir)
    with open(os.path.join(outdir, 'xamgen.json')) as f:
        console_only = json.load(f).get('console_only')
    if console_only:
        sys.exit('xenia: refusing a build with console-only scenarios, '
                 'build it with --only to leave them out\n  ' +
                 '\n  '.join(console_only))
    if not os.path.exists(XENIA):
        sys.exit(f'xenia: no {XENIA}, set XENIA_EXE to the build to run')
    xex = os.path.join(outdir, 'xamgen.xex')
    log = os.path.join(outdir, 'xenia.log')
    results = os.path.join(outdir, 'xamgen.bin')
    for path in (results, log):
        if os.path.exists(path):
            os.remove(path)
    before = windows_pids(XENIA) if from_wsl(XENIA) else set()
    started = time.time()
    process = subprocess.Popen(
        [XENIA, *XENIA_ARGS, f'--log_file={host_path(log, XENIA)}',
         host_path(xex, XENIA)],
        cwd=os.path.dirname(XENIA), stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL)
    deadline = started + timeout
    summary = None
    last_step = None
    try:
        while time.time() < deadline and process.poll() is None:
            time.sleep(0.5)
            if not os.path.exists(log):
                continue
            with open(log, errors='replace') as f:
                for line in f:
                    if '(DbgPrint) xamgen:' in line:
                        summary = line.strip()
                    elif '(DbgPrint) xamgen step ' in line:
                        last_step = int(line.split('xamgen step ')[1])
            if summary and os.path.exists(results):
                time.sleep(0.5)
                break
    finally:
        exit_code = process.poll()
        if from_wsl(XENIA):
            for pid in windows_pids(XENIA) - before:
                subprocess.run(['taskkill.exe', '/PID', str(pid), '/F'],
                               capture_output=True)
        elif exit_code is None:
            process.kill()
        process.wait(timeout=10)
    if not summary:
        with open(os.path.join(outdir, 'xamgen.json')) as f:
            run = [s for s in json.load(f)['steps'] if not s.get('faults')]
        step = (f'{run[last_step]["scenario"]} / {run[last_step]["name"]}'
                if last_step is not None else 'none')
        # The log trails behind, so the step that died can be a later one.
        where = f'last step logged {last_step}: {step}'
        folder, dumps = crash_dumps(started)
        if dumps:
            sys.exit(f'xenia crashed, {where}\ndump {dumps[0]} in {folder}')
        if exit_code is not None:
            sys.exit(f'xenia exited with {exit_code} before the summary, '
                     f'{where}, see {log}')
        sys.exit(f'xenia: no summary within {timeout} s, {where}, see {log}')
    print(f'xenia: {summary}')
    copy = os.path.join(outdir, 'xenia_results.bin')
    os.replace(results, copy)
    return decode_into(outdir, 'xenia', copy)


def watch_events(path):
    events = []
    if os.path.exists(path):
        with open(path, encoding='utf-8-sig', errors='replace') as f:
            for line in f:
                try:
                    events.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
    return events


def run_console(outdir, timeout):
    outdir = os.path.abspath(outdir)
    with open(os.path.join(outdir, 'xamgen.json')) as f:
        meta = json.load(f)
    xex = f'{CONSOLE_DIR}\\xamgen.xex'
    results = f'{CONSOLE_DIR}\\xamgen.bin'
    code, out = rgh('title')
    if 'Aurora' not in out:
        sys.exit('console: not at Aurora, refusing to launch')
    code, out = rgh('fs', 'put', '--path', xex, '--in',
                    host_path(os.path.join(outdir, 'xamgen.xex'), rgh_exe()),
                    timeout=180)
    if code:
        sys.exit(f'console: upload failed\n{out}')
    rgh('fs', 'rm', '--path', results)
    code, out = rgh('launch', '--xex', xex)
    if code:
        sys.exit(f'console: launch failed\n{out}')
    if meta.get('unwatched'):
        wait_unwatched(meta, timeout)
    else:
        watch(meta, outdir, timeout)
    for _ in range(30):
        code, out = rgh('title')
        if 'Aurora' in out:
            break
        time.sleep(1)
    copy = os.path.join(outdir, 'console_results.bin')
    code, out = rgh('fs', 'get', '--path', results, '--out',
                    host_path(copy, rgh_exe()))
    if code:
        sys.exit(f'console: no results file\n{out}')
    return decode_into(outdir, 'console', copy)


def peek_u32(address):
    """The console's word at `address`, or None if XBDM can't read it."""
    code, out = rgh('mem', 'peek', '--addr', f'{address:#x}', '--type', 'u32')
    values = [t for t in out.split()
              if re.fullmatch(r'0x[0-9A-Fa-f]+|\d+', t)]
    return int(values[-1], 0) if not code and values else None


def fail_at_progress(meta, what):
    """Names the step the executor's progress word is at, goes back to Aurora
    and exits."""
    progress = peek_u32(meta['control'] + xamgen.C_PROGRESS)
    rgh('launch', '--xex', AURORA)
    run = [s for s in meta['steps'] if not s.get('faults')]
    if progress is not None and progress >= len(run):
        sys.exit(f'console: {what}\nafter the last step, writing the '
                 f'results or exiting')
    step = run[progress] if progress is not None else None
    sys.exit(f'console: {what}\nprogress {progress}: '
             f'{step["scenario"] + " / " + step["name"] if step else "?"}'
             f'\nmark it faults=True and rebuild')


def wait_unwatched(meta, timeout):
    """Polls the executor's done word, then writes the exit word. A fault no
    handler takes stops the title, so the progress word names the step when
    the executor isn't done in time."""
    control = meta['control']
    deadline = time.time() + timeout
    started = False
    while time.time() < deadline:
        code, out = rgh('title')
        if 'xamgen.xex' not in out.lower():
            if started:
                sys.exit('console: the title ended before it was done')
            time.sleep(0.5)
            continue
        started = True
        if peek_u32(control + xamgen.C_DONE) == meta['done_magic']:
            status = peek_u32(control + xamgen.C_FILE_STATUS)
            rgh('mem', 'poke', '--addr', f'{control + meta["exit_offset"]:#x}',
                '--type', 'u32', '--value', f'{meta["exit_magic"]:#x}')
            if status != 0:
                sys.exit(f'console: no results file, status {status}')
            print('console: done')
            return
        time.sleep(0.5)
    fail_at_progress(meta, f'not done after {timeout} s')


def watch(meta, outdir, timeout):
    """Watches XBDM's notifications until the executor unloads or faults."""
    watch_log = os.path.join(outdir, 'console_watch.jsonl')
    if os.path.exists(watch_log):
        os.remove(watch_log)
    watcher = subprocess.Popen(
        [rgh_exe(), 'debug', 'watch', '--duration', str(timeout), '--raw',
         '--out', host_path(watch_log, rgh_exe())], stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL)
    deadline = time.time() + timeout
    fault = None
    summary = None
    try:
        while time.time() < deadline and watcher.poll() is None:
            time.sleep(0.5)
            events = watch_events(watch_log)
            for event in events:
                raw = event.get('Raw', '')
                if event.get('Type') == 'debugstr' and 'xamgen:' in raw:
                    summary = raw
                if event.get('Type') == 'exception':
                    fault = raw
            if fault or any(e.get('Type') == 'modunload' and
                            'xamgen.xex' in e.get('Raw', '') for e in events):
                break
    finally:
        watcher.terminate()
        watcher.wait()
    if fault:
        fail_at_progress(meta, fault)
    # Only this run's summary ties the results file to it.
    written = re.search(r'results file ([0-9A-Fa-f]{8})', summary or '')
    if not written or int(written.group(1), 16):
        sys.exit(f'console: no results from this run, summary {summary!r}')
    print(f'console: {summary}')


def identities(results):
    """Hex of the values that differ by console or profile, to their names."""
    names = {}
    for step in results:
        if step['scenario'] != 'identity':
            continue
        capture = step.get('capture', {})
        if 'xuid' in capture:
            names[capture['xuid']] = '<X0>'
        if 'gamertag' in capture:
            gamertag = bytes.fromhex(capture['gamertag']).split(b'\0')[0]
            if gamertag:
                names[gamertag.hex()] = '<NAME0>'
    return names


def truncated_names(value, names):
    """A gamertag a short buffer cut, as <NAME0:length>."""
    for raw, name in names:
        if not name.startswith('<NAME'):
            continue
        for length in range(len(raw) // 2 - 1, 0, -1):
            value = value.replace(raw[:2 * length] + '00',
                                  f'{name[:-1]}:{length}>00')
    return value


def normalize(results):
    names = sorted(identities(results).items(), key=lambda n: -len(n[0]))
    steps = []
    for step in results:
        out = {'scenario': step['scenario'], 'name': step['name'],
               'export': step['export']}
        if step.get('faults'):
            out['faults'] = True
            steps.append(out)
            continue
        if 'r3' in step:
            out['r3'] = step['r3']
        if 'wait_left' in step:
            out['completed'] = step['wait_left'] > 0
        capture = {}
        for key, value in step.get('capture', {}).items():
            for raw, name in names:
                value = value.replace(raw, name)
            if step['export'] == 'XamUserGetName':
                value = truncated_names(value, names)
            # The sentinel left after a name depends on the name's length.
            value = re.sub(r'(<NAME0(?::\d+)?>00)(?:cd)*', r'\1<fill>', value)
            capture[key] = value
        if capture:
            out['capture'] = capture
        steps.append(out)
    return steps


def load_results(path):
    with open(path) as f:
        decoded = json.load(f)
    if decoded['steps_done'] < decoded['steps_run']:
        print(f'{path}: only {decoded["steps_done"]} of '
              f'{decoded["steps_run"]} steps done')
    return decoded


def by_scenario(steps):
    scenarios = {}
    for step in steps:
        scenarios.setdefault(step['scenario'], []).append(step)
    return scenarios


def comparable(step):
    """The step with r3 cut to its low 32 bits when the high half is only their
    zero or sign extension.

    Xenia sign-extends every 32-bit result and a console extends it either way,
    depending on the instruction that made it. Titles read the low half only.
    Caveat: this also hides a real difference in an export that returns 64 bits
    with a high half of 0, or of all ones over a low half with bit 31 set.
    """
    if 'r3' not in step:
        return step
    value = int(step['r3'], 16)
    high, low = value >> 32, value & 0xFFFFFFFF
    if high == 0 or (high == 0xFFFFFFFF and low & 0x80000000):
        return dict(step, r3=f'{low:08X}')
    return step


def compare(expected, actual):
    """Prints the steps that differ, returns how many do and how many were
    compared. Scenarios a run left out (a build with --only) count as not
    run."""
    want_scenarios = by_scenario(expected)
    got_scenarios = by_scenario(actual)
    for scenario in got_scenarios.keys() - want_scenarios.keys():
        sys.exit(f'{scenario}: not in the expected results, record them again')
    same = differ = not_run = 0
    for scenario, wants in want_scenarios.items():
        gots = got_scenarios.get(scenario)
        if gots is None:
            not_run += sum(1 for s in wants if not s.get('faults'))
            continue
        if [s['name'] for s in wants] != [s['name'] for s in gots]:
            sys.exit(f'{scenario}: the runs come from different corpora')
        for want, got in zip(wants, gots):
            if want.get('faults') and got.get('faults'):
                continue
            want_cmp, got_cmp = comparable(want), comparable(got)
            if want_cmp == got_cmp:
                same += 1
                continue
            differ += 1
            print(f'{scenario} / {want["name"]}')
            for key in ('r3', 'completed', 'capture'):
                if want_cmp.get(key) != got_cmp.get(key):
                    print(f'  {key}: {want.get(key)}\n  {" " * len(key)}  '
                          f'{got.get(key)}')
    print(f'{same} same, {differ} different' +
          (f', {not_run} not run' if not_run else ''))
    return differ, same + differ


def diff(a_path, b_path):
    compare(normalize(load_results(a_path)['results']),
            normalize(load_results(b_path)['results']))


def expect(results_path, expected_path, source):
    decoded = load_results(results_path)
    with open(os.path.join(os.path.dirname(results_path),
                           'xamgen.json')) as f:
        meta = json.load(f)
    steps = normalize(decoded['results'])
    for step, described in zip(steps, meta['steps']):
        step['args'] = described['args']
    with open(expected_path, 'w') as f:
        json.dump({'corpus': decoded['corpus'], 'source': source,
                   'steps': steps}, f, indent=1)
        f.write('\n')
    print(f'{len(steps)} steps -> {expected_path}')


def check(expected_path, results_path):
    with open(expected_path) as f:
        expected = json.load(f)
    for step in expected['steps']:
        step.pop('args', None)
    results = load_results(results_path)
    if results['corpus'] != expected['corpus']:
        sys.exit(f'{results_path} is a run of {results["corpus"]}, not '
                 f'{expected["corpus"]}')
    differ, compared = compare(expected['steps'],
                               normalize(results['results']))
    if differ or not compared or results['steps_done'] < results['steps_run']:
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = parser.add_subparsers(dest='command', required=True)
    for name in ('xenia', 'console'):
        p = sub.add_parser(name)
        p.add_argument('outdir')
        p.add_argument('--timeout', type=int, default=30)
    p = sub.add_parser('diff')
    p.add_argument('a')
    p.add_argument('b')
    p = sub.add_parser('expect')
    p.add_argument('results')
    p.add_argument('expected')
    p.add_argument('--source', required=True,
                   help='where the run came from, e.g. the console and xam')
    p = sub.add_parser('check')
    p.add_argument('expected')
    p.add_argument('results')
    args = parser.parse_args()
    if args.command == 'xenia':
        run_xenia(args.outdir, args.timeout)
    elif args.command == 'console':
        run_console(args.outdir, args.timeout)
    elif args.command == 'expect':
        expect(args.results, args.expected, args.source)
    elif args.command == 'check':
        check(args.expected, args.results)
    else:
        diff(args.a, args.b)


if __name__ == '__main__':
    main()
