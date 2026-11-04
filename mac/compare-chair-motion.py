#!/usr/bin/env python3
"""Start the read-only inbound and USB observers together and label their output on one host clock."""
import os
import queue
import re
import subprocess
import sys
import threading
import time

def collect(name, process, events):
    try:
        with process.stdout:
            for line in process.stdout:
                events.put((name, line.rstrip("\n")))
    finally:
        events.put((name, None))

def compare(commands, seconds):
    start = time.monotonic()
    events = queue.Queue()
    processes = {}
    finished = set()
    neutral = baseline = ready = False
    try:
        for name, command in commands:
            child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
            processes[name] = child
            threading.Thread(target=collect, args=(name, child, events), daemon=True).start()
        while len(finished) != len(processes):
            remaining = start + seconds + 8 - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("Observer exceeded the capture time limit")
            try:
                name, line = events.get(timeout=remaining)
            except queue.Empty:
                raise RuntimeError("No observer result before the time limit")
            if line is None:
                finished.add(name)
                if processes[name].wait(timeout=1) != 0:
                    raise RuntimeError(name + " observation failed; this comparison is incomplete")
                continue
            print("%7.3fs %-7s %s" % (time.monotonic()-start, name, line), flush=True)
            baseline |= name == "CHAIR" and line.startswith("Inbound chair baseline recorded.")
            neutral |= name == "USB" and re.search(r"USB INPUT x=0 y=0(?: buttons=0x000)?$", line) is not None
            if neutral and baseline and not ready:
                ready = True
                print("READY: both observers are receiving. Try the one requested direction and release.", flush=True)
        if not ready:
            raise RuntimeError("Both a fresh chair baseline and actual neutral USB input are required")
    finally:
        for process in processes.values():
            if process.poll() is None:
                process.terminate()
        for process in processes.values():
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

def main(args):
    if args == ["--self-test"]:
        def simulated(text, code=0):
            return [sys.executable, "-u", "-c", "print(%r);raise SystemExit(%d)" % (text, code)]
        pair = [("USB", simulated("1 ms USB INPUT x=0 y=0")), ("CHAIR", simulated("Inbound chair baseline recorded."))]
        compare(pair, 2)
        for broken in [
            [("USB", simulated("no reports")), pair[1]],
            [("USB", simulated("failure",1)), pair[1]]
        ]:
            try:
                compare(broken, 2)
            except RuntimeError:
                continue
            raise RuntimeError("Failed helper incorrectly counted as complete")
        print("PASS: 3 comparison-process checks. Synthetic child output only.")
        return
    if (len(args) != 4 or not re.fullmatch(r"0x[0-9a-fA-F]{1,8}",args[2])
            or int(args[2],16) == 0 or not re.fullmatch(r"[0-9]{1,3}",args[3])
            or not 2 <= int(args[3]) <= 180):
        raise ValueError("Usage: compare-chair-motion.py /path/omni-maintenance /path/omni-observer 0xLOCATION SECONDS")
    maintenance, observer, location, seconds = args
    for path, binary in [(maintenance,"omni-maintenance"),(observer,"omni-observer")]:
        if not os.path.isabs(path) or os.path.basename(path)!=binary or not os.access(path, os.X_OK):
            raise ValueError("Supply the absolute path of the built " + binary)
    compare([("USB",[observer,"--watch",seconds]), ("CHAIR",[maintenance,"--watch-chair-motion",location,seconds])],int(seconds))

if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(1)
