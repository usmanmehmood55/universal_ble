"""Close the HIL app while a WinRT operation is pending; time the exit.

Builds `lib/close_scenario.dart` once per scenario, starts it, waits until
it reports that its operation is pending, posts WM_CLOSE and measures how long
the process takes to exit. Closing must not wait for the pending operation,
which the fixture holds for 15 seconds.

Needs the fixture in range with contract revision 2 firmware with opcodes
0x0d and 0x18.

    python close_hil_test.py --runs 3
"""

import argparse
import ctypes
import os
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes
from pathlib import Path

HERE = Path(__file__).resolve().parent
EXE = HERE / "build" / "windows" / "x64" / "runner" / "Release" / "universal_ble_hil.exe"
SCENARIOS = ("connected", "connect", "subscribe")
# The #375 fix bounds connection cleanup at 3 s; allow for process teardown.
MAX_EXIT_SECONDS = 5.0

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
enum_callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumWindows.argtypes = [enum_callback, wintypes.LPARAM]


def find_window(pid):
    found = []

    @enum_callback
    def visit(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        name = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, name, len(name))
        if owner.value == pid and name.value == "FLUTTER_RUNNER_WIN32_WINDOW":
            found.append(hwnd)
            return False
        return True

    user32.EnumWindows(visit, 0)
    return found[0] if found else None


def build(scenario, ready_file):
    subprocess.run(
        ["flutter", "build", "windows", "--release", "--no-pub",
         "-t", "lib/close_scenario.dart",
         f"--dart-define=HIL_CLOSE_SCENARIO={scenario}",
         f"--dart-define=HIL_READY_FILE={ready_file}"],
        cwd=HERE, check=True, shell=True,
        stdout=subprocess.DEVNULL)


def run_once(ready_file):
    ready = Path(ready_file)
    ready.unlink(missing_ok=True)
    process = subprocess.Popen([str(EXE)], env=os.environ)
    try:
        deadline = time.monotonic() + 90
        while not ready.exists():
            if process.poll() is not None:
                return f"EXITED_EARLY:{process.returncode}"
            if time.monotonic() > deadline:
                return "NOT_READY"
            time.sleep(0.05)
        time.sleep(0.1)
        state = ready.read_text()
        if state != "ready":
            return f"SCENARIO_FAILED:{state}"
        window = find_window(process.pid)
        if not window:
            return "NO_WINDOW"
        closed = time.monotonic()
        if not user32.PostMessageW(window, 0x0010, 0, 0):
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            return "HUNG"
        return f"{time.monotonic() - closed:.2f}s"
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--scenarios", default=",".join(SCENARIOS))
    args = parser.parse_args()
    ready_file = os.path.join(tempfile.gettempdir(), "hil_close_ready.txt")
    failed = False
    for scenario in args.scenarios.split(","):
        build(scenario, ready_file)
        results = []
        for _ in range(args.runs):
            result = run_once(ready_file)
            results.append(result)
            ok = result.endswith("s") and float(result[:-1]) <= MAX_EXIT_SECONDS
            failed |= not ok
            # The next run resets the fixture, which ends any pause or delay.
            time.sleep(1)
        print(f"{scenario}: {results}", flush=True)
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
