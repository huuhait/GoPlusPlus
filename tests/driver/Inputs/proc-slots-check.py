"""For proc-slots.test: PROCS processes of THREADS threads each hold a slot
(tests/procslots.py) for 10 ms, TIMES times per thread; prints how many holds
there were and how many were held at once at most (CLOCK_MONOTONIC, the same
clock in every process of the machine).

usage: proc-slots-check.py PROCS THREADS TIMES
"""
import os
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
import procslots  # noqa: E402


def load(threads, times):
    spans = []
    lock = threading.Lock()

    def work():
        for _ in range(times):
            with procslots.slot():
                t0 = time.monotonic()
                time.sleep(0.01)
                t1 = time.monotonic()
            with lock:
                spans.append((t0, t1))

    ts = [threading.Thread(target=work) for _ in range(threads)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    for t0, t1 in spans:
        print("%.6f %.6f" % (t0, t1))


def main():
    if sys.argv[1] == "--load":
        load(int(sys.argv[2]), int(sys.argv[3]))
        return
    procs, threads, times = map(int, sys.argv[1:4])
    ps = [subprocess.Popen([sys.executable, __file__, "--load", str(threads), str(times)],
                           stdout=subprocess.PIPE, text=True) for _ in range(procs)]
    events = []
    for p in ps:
        out, _ = p.communicate()
        if p.returncode:
            sys.exit("a load process failed")
        for line in out.splitlines():
            t0, t1 = map(float, line.split())
            events += [(t0, 1), (t1, -1)]
    # An end before a start at the same instant: a slot given back and taken
    # again.
    events.sort()
    now = most = 0
    for _, d in events:
        now += d
        most = max(most, now)
    print("spans: %d" % (len(events) // 2))
    print("most at once: %d" % most)


if __name__ == "__main__":
    main()
