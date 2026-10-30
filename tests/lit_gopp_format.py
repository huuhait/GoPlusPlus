"""The lit test format of the Go++ tests: ShTest, plus the batch run of the
programs of the run tests through `gopp test-run` (P04.19, D4.14). See the
comment at the end of tests/lit.cfg.py for what it does and how to switch it
off."""
import json
import os
import resource
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

import lit.Test
import lit.TestRunner
import lit.formats


# The batch is the whole run corpus (about 0.2 s of wall time today): five
# minutes is far over any healthy run. Each program has the time limit of the
# tests; one stream of one program is cut at 16 MiB (SIGXFSZ), so a program
# that prints for ever fills neither the disk nor the runner's memory.
BATCH_TIMEOUT_SECONDS = 300
MAX_OUTPUT_BYTES = 16 << 20


class GoppTestFormat(lit.formats.ShTest):
    """ShTest, plus the batch run of tests/run (see tests/lit.cfg.py)."""

    def __init__(self, source_dir, binary_dir, tools_dir, run_long):
        super().__init__(execute_external=False)
        self.source_dir = source_dir
        self.binary_dir = binary_dir
        self.tools_dir = tools_dir
        self.run_long = run_long

    def getTestsInDirectory(self, testSuite, path_in_suite, litConfig, localConfig):
        tests = list(
            super().getTestsInDirectory(testSuite, path_in_suite, litConfig, localConfig)
        )
        if tuple(path_in_suite) == ("run",) and not localConfig.environment.get(
            "GOPP_TEST_REAL"
        ):
            self.batch(tests, litConfig, localConfig)
        return tests

    @staticmethod
    def lit_options():
        """lit's own command line (arguments and LIT_OPTS), parsed again by
        lit's parser: the discovery gets no -j or --show-tests from lit. None
        if it cannot be read."""
        try:
            import lit.cl_arguments

            return lit.cl_arguments.parse_args()
        except (Exception, SystemExit):
            return None

    def batch(self, tests, litConfig, localConfig):
        opts = self.lit_options()
        if opts and (opts.show_tests or opts.show_suites):
            return  # only listing: nothing runs
        gopp = os.path.join(self.tools_dir, "gopp")
        os.environ["GOPP"] = gopp
        sys.path.insert(0, os.path.join(self.source_dir, "tests", "run", "Inputs"))
        import runlib

        # Normal tests and long tests never run in the same lit run
        # (check-long filters on the name), so only the ones that will run.
        want_long = self.run_long
        jobs = {}  # key -> sources
        for t in tests:
            if t.path_in_suite[-1].endswith(".long.test") != want_long:
                continue
            script = lit.TestRunner.parseIntegratedTestScript(t)
            if isinstance(script, lit.Test.Result):
                continue  # unsupported here
            tmpDir, tmpBase = lit.TestRunner.getTempPaths(t)
            subs = lit.TestRunner.getDefaultSubstitutions(t, tmpDir, tmpBase)
            conds = {f: True for f in t.config.available_features}
            script = lit.TestRunner.applySubstitutions(
                script, subs, conds, recursion_limit=t.config.recursiveExpansionLimit
            )
            for line in script:
                try:
                    toks = shlex.split(line)
                except ValueError:
                    continue  # not a command line this batch understands
                for i, tok in enumerate(toks):
                    if os.path.basename(tok).startswith("run-") and tok.endswith(".py"):
                        end = next((j for j in range(i, len(toks)) if toks[j] in ("|", "&&", ";", "||")), len(toks))
                        for srcs in runlib.plan(tok, toks[i + 1:end]):
                            if srcs and all(os.path.exists(x) for x in srcs):
                                jobs[runlib.job_key(srcs)] = srcs
                        break
        if not jobs:
            return
        root = os.path.join(self.binary_dir, "tests", "fastrun")
        os.makedirs(root, exist_ok=True)
        # Directories of runs that ended long ago. Another lit run on the same
        # build directory may remove the same ones at the same time: a
        # directory that is gone is not an error.
        now = time.time()
        try:
            names = os.listdir(root)
        except OSError:
            names = []
        for d in names:
            p = os.path.join(root, d)
            try:
                if now - os.path.getmtime(p) > 3600:
                    shutil.rmtree(p, ignore_errors=True)
            except OSError:
                pass
        out_dir = tempfile.mkdtemp(prefix="run-", dir=root)
        manifest = os.path.join(out_dir, "manifest.jsonl")
        limit = litConfig.maxIndividualTestTime or 60
        with open(manifest, "w") as f:
            for key, srcs in jobs.items():
                f.write(json.dumps({"v": runlib.PROTOCOL_VERSION, "id": key, "inputs": srcs,
                                    "timeout_ms": int(min(limit, 60) * 1000)}) + "\n")
        # The environment of the tests, not lit's own: a real binary gets
        # config.environment only, so the fast run must see the same.
        env = dict(localConfig.environment)
        results = os.path.join(out_dir, "results.jsonl")
        # The objects and output files of the batch are kept here, so they go
        # when this run is done (and with out_dir when a later run cleans up),
        # also when gopp is killed and cannot clean up itself.
        work_dir = os.path.join(out_dir, "work")
        # Few executors for a few workers: lit's own -j says how much of the
        # machine this run may use (a quarter for executors, as the default).
        workers = max(opts.workers if opts and opts.workers else os.cpu_count() or 1, 1)
        executors = min(max(workers // 4, 2), 16)
        # TEST ONLY (tests/run/runner-batch-fallback.test): another command in
        # place of `gopp`, to make the batch fail in each way it can.
        cmd = shlex.split(litConfig.params.get("fastrun_batch_gopp", "")) or [gopp]
        t0 = time.time()
        cpu0 = resource.getrusage(resource.RUSAGE_CHILDREN)
        failure = None
        try:
            p = subprocess.run(
                cmd + ["test-run", "-manifest", manifest, "-o", results,
                       "-work-dir", work_dir, "-j", str(workers),
                       "-executors", str(executors),
                       "-max-output", str(MAX_OUTPUT_BYTES)],
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                env=env, timeout=BATCH_TIMEOUT_SECONDS)
            if p.returncode != 0:
                failure = "exit status %d: %s" % (
                    p.returncode, p.stderr.decode(errors="replace").strip()[:300])
        except (OSError, subprocess.SubprocessError) as e:
            failure = str(e)
        shutil.rmtree(work_dir, ignore_errors=True)
        cpu1 = resource.getrusage(resource.RUSAGE_CHILDREN)
        wall = time.time() - t0
        cpu = (cpu1.ru_utime - cpu0.ru_utime) + (cpu1.ru_stime - cpu0.ru_stime)
        if failure is None:
            try:
                self.save_results(results, out_dir, jobs, runlib.PROTOCOL_VERSION)
            except (OSError, ValueError, AttributeError, TypeError) as e:
                failure = "unreadable results: %s" % e
        if failure is None:
            localConfig.environment["GOPP_FASTRUN_DIR"] = out_dir
        else:
            # Nothing of this batch is used: every test runs its program
            # alone (runlib.run_sources), as when lit runs one test.
            litConfig.warning("the batch run of the run tests failed (%s); "
                              "each test runs its program alone" % failure)
        # The suite-time gate counts this work too (tools/check-suite-time.py).
        stats = os.environ.get("GOPP_FASTRUN_STATS")
        if stats:
            with open(stats, "w") as f:
                json.dump({"programs": len(jobs), "wall_seconds": wall, "cpu_seconds": cpu}, f)

    @staticmethod
    def save_results(results, out_dir, jobs, version):
        """Splits the batch's results into <key>.json files for the tests.
        All or nothing: a line that is not a result of this batch (bad JSON,
        another protocol version, a key that was not asked for) means the
        batch cannot be trusted, and the caller falls back."""
        lines = {}
        with open(results) as f:
            for line in f:
                if not line.strip():
                    continue
                d = json.loads(line)
                if d.get("v") != version or d.get("id") not in jobs:
                    raise ValueError("not a result of this batch: %r" % line[:80])
                lines[d["id"]] = line
        for key, line in lines.items():
            with open(os.path.join(out_dir, key + ".json"), "w") as g:
                g.write(line)
