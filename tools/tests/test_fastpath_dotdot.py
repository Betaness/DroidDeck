import os
from pathlib import Path
import random
import shutil
import subprocess
import tempfile
import time
import unittest

FASTPATH = Path(__file__).resolve().parents[1] / "proot/fastpath/fastpath.c"

PROGRAM = r"""
#include "FASTPATH"

static unsigned long calls, xattr_calls;

static long host_sys(long nr, long a, long b, long c, long d, long e, long f) {
  calls++;
  if (nr == SYS_getxattr || nr == SYS_lgetxattr) xattr_calls++;
  long r = syscall(nr, a, b, c, d, e, f);
  return r < 0 ? -errno : r;
}

static void show(long r, const struct stat *st) {
  if (r == FP_SLOW) puts("SLOW");
  else if (r < 0) printf("E%ld\n", -r);
  else printf("%llu %llu %c\n", (unsigned long long)st->st_dev, (unsigned long long)st->st_ino,
              S_ISDIR(st->st_mode) ? 'd' : S_ISREG(st->st_mode) ? 'f' : S_ISLNK(st->st_mode) ? 'l' : 'o');
}

static long raw(long r) { return r < 0 ? -errno : r; }

int main(int argc, char **argv) {
  static char hosts[MAX_BINDS][PATH_MAX], guests[MAX_BINDS][512];
  fp_sys = host_sys;
  snprintf(root, sizeof root, "%s", argv[1]);
  rootlen = strlen(root);
  int i = 2;
  for (; i + 1 < argc && strcmp(argv[i], "--"); i += 2) {
    struct bind *bd = &binds[nbinds];
    bd->host = hosts[nbinds];
    bd->guest = guests[nbinds];
    snprintf(bd->host, PATH_MAX, "%s", argv[i]);
    snprintf(bd->guest, sizeof guests[0], "%s", argv[i + 1]);
    bd->hlen = strlen(bd->host);
    bd->glen = strlen(bd->guest);
    nbinds++;
  }
  pthread_once(&init_once, fp_init);
  fp_on = 1;
  for (i++; i < argc; i++) {
    const char *op = argv[i];
    struct stat st;
    if (!strcmp(op, "stat")) {
      int flags = !strcmp(argv[i + 1], "L") ? AT_SYMLINK_NOFOLLOW : 0;
      long r = fp_stat(AT_FDCWD, argv[i + 2], flags, &st);
      i += 2;
      show(r, &st);
    } else if (!strcmp(op, "ref")) {
      int flags = !strcmp(argv[i + 1], "L") ? AT_SYMLINK_NOFOLLOW : 0;
      long r = raw(syscall(SYS_newfstatat, AT_FDCWD, argv[i + 2], &st, flags));
      i += 2;
      show(r, &st);
    } else if (!strcmp(op, "getx")) {
      long r = fp_getxattr(argv[i + 2], argv[i + 3], NULL, 0, !strcmp(argv[i + 1], "F"));
      i += 3;
      if (r == FP_SLOW) puts("SLOW");
      else if (r < 0) printf("E%ld\n", -r);
      else printf("%ld\n", r);
    } else if (!strcmp(op, "rawx")) {
      // getxattr through the raw-syscall dispatcher; name "bad" is an unmapped pointer, "edge" an
      // unterminated "user." name that ends at an unmapped page.
#if defined(__aarch64__)
      long reg[9] = {0}, pg = sysconf(_SC_PAGESIZE);
      char *edge = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      munmap(edge + pg, pg);
      memcpy(edge + pg - 8, "user.abc", 8);
      reg[0] = (long)argv[i + 2];
      reg[1] = !strcmp(argv[i + 3], "bad") ? 8 : !strcmp(argv[i + 3], "edge") ? (long)(edge + pg - 8) : (long)argv[i + 3];
      reg[8] = !strcmp(argv[i + 1], "F") ? SYS_getxattr : SYS_lgetxattr;
      long r = svc_path_call(reg);
      if (r == FP_SLOW) puts("SLOW");
      else if (r < 0) printf("E%ld\n", -r);
      else printf("%ld\n", r);
      munmap(edge, pg);
#else
      puts("NOSVC");
#endif
      i += 3;
    } else if (!strcmp(op, "count")) {
      printf("%lu %lu\n", calls, xattr_calls);
      calls = xattr_calls = 0;
    } else if (!strcmp(op, "dotdot")) {
      dotdot_fast = atoi(argv[++i]);
    } else if (!strcmp(op, "xattr")) {
      xattr_absent_cache = atoi(argv[++i]);
    } else if (!strcmp(op, "ttl")) {
      ttl_ns = atoll(argv[++i]) * 1000000LL;
    } else if (!strcmp(op, "nap")) {
      usleep(atoi(argv[++i]) * 1000);
    } else if (!strcmp(op, "cd")) {
      if (syscall(SYS_chdir, argv[++i]) != 0) return 3;
    } else {
      long r;
      if (!strcmp(op, "rmdir")) r = syscall(SYS_unlinkat, AT_FDCWD, argv[++i], AT_REMOVEDIR);
      else if (!strcmp(op, "mkdir")) r = syscall(SYS_mkdirat, AT_FDCWD, argv[++i], 0755);
      else if (!strcmp(op, "unlink")) r = syscall(SYS_unlinkat, AT_FDCWD, argv[++i], 0);
      else if (!strcmp(op, "rename")) { r = syscall(SYS_renameat, AT_FDCWD, argv[i + 1], AT_FDCWD, argv[i + 2]); i += 2; }
      else if (!strcmp(op, "symlink")) { r = syscall(SYS_symlinkat, argv[i + 1], AT_FDCWD, argv[i + 2]); i += 2; }
      else if (!strcmp(op, "touch")) {
        r = syscall(SYS_openat, AT_FDCWD, argv[++i], O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (r >= 0) r = syscall(SYS_close, r);
      } else if (!strcmp(op, "setx")) {
        r = syscall(SYS_setxattr, argv[i + 1], argv[i + 2], argv[i + 3], strlen(argv[i + 3]), 0);
        i += 3;
      } else if (!strcmp(op, "remx")) { r = syscall(SYS_removexattr, argv[i + 1], argv[i + 2]); i += 2; }
      else return 2;
      if (r < 0) { fprintf(stderr, "%s: %s\n", op, strerror(errno)); return 4; }
    }
    fflush(stdout);
  }
  return 0;
}
"""

# Guest paths with ".." and the host path whose stat the kernel gives for them (None: no answer).
DOTDOT = [
    ("/d/..", ""),
    ("/d/e/..", "d"),
    ("/d/e/../g", "d/g"),
    ("/d/e/../../file", "file"),
    ("/d/./..", ""),
    ("/d/../", ""),
    ("/d/e/../..", ""),
    ("/d/e/h/../../g/..", "d"),
    ("/rel/..", "d"),
    ("/rel/../g", "d/g"),
    ("/chain/..", "d"),
    ("/abs/..", "d"),
    ("/up/..", ""),
    ("/s/lnk/..", "s/t"),
    ("/fl/..", None),
    ("/file/..", None),
    ("/missing/..", None),
    ("/d/missing/..", None),
    ("/dangling/..", None),
    ("/loop1/..", None),
    ("/..", ""),
    ("/../..", ""),
    ("/../d", "d"),
    ("/d/../..", ""),
    ("/deep/x/y/z/..", "deep/x/y"),
    ("/deep/x/y/z/../../..", "deep"),
    ("/deep/x/y/w/..", None),
    ("/mnt/sd/..", "mnt"),
    ("/mnt/sd/sub/..", "@lib"),
    ("/mnt/sd/out/..", ""),
    ("/proc/self/..", "@/proc"),
    ("/proc/self/fd/..", None),
]

RELATIVE = [("e/..", "d"), ("..", ""), ("../d/e/..", "d"), ("e/h/../..", "d"), ("./e/./..", "d")]


class FastPathDotDotTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if shutil.which("gcc") is None:
            raise unittest.SkipTest("gcc is not available")
        cls.work = Path(tempfile.mkdtemp())
        t = cls.work / "tree"
        for d in ["root/x", "lib/sub"]:
            (t / d).mkdir(parents=True)
        for f in ["root/x/old", "root/x/old2"]:
            (t / f).touch()
        os.symlink("old", t / "root/x/link")
        cls.xattr_since = time.monotonic()
        source = cls.work / "dotdot.c"
        source.write_text(PROGRAM.replace("FASTPATH", str(FASTPATH)))
        cls.program = cls.work / "dotdot"
        subprocess.run(["gcc", "-O1", "-w", "-pthread", "-o", str(cls.program), str(source), "-ldl"], check=True)
        r = t / "root"
        for d in ["d/e/h", "d/g", "deep/x/y/z", "mnt", "s/p", "s/t/q", "s/other/q"]:
            (r / d).mkdir(parents=True, exist_ok=True)
        for f in ["d/e/f", "file", "deep/x/y/w"]:
            (r / f).touch()
        os.symlink("d/e", r / "rel")
        os.symlink("rel", r / "chain")
        os.symlink("/d/e", r / "abs")
        os.symlink("file", r / "fl")
        os.symlink("nope", r / "dangling")
        os.symlink("d/e/..", r / "up")
        os.symlink("loop2", r / "loop1")
        os.symlink("loop1", r / "loop2")
        os.symlink("t/q", r / "s/lnk")
        os.symlink("..", t / "lib/out")
        cls.tree = t
        cls.root = r
        cls.binds = [(t / "lib", "/mnt/sd"), ("/proc", "/proc")]

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work, ignore_errors=True)

    def ask(self, *queries, env=None):
        argv = [str(self.program), str(self.root)]
        for host, guest in self.binds:
            argv += [str(host), guest]
        argv.append("--")
        for q in queries:
            argv += [str(x) for x in q]
        out = subprocess.run(argv, capture_output=True, text=True, timeout=20, check=True,
                             env=dict(os.environ, **(env or {}))).stdout
        return out.splitlines()

    def host(self, rel):
        if rel is None:
            return None
        if rel.startswith("@/"):
            return Path(rel[1:])
        if rel.startswith("@"):
            return self.tree / rel[1:]
        return self.root / rel if rel else self.root

    def kernel(self, rel, follow=True):
        st = os.stat(self.host(rel), follow_symlinks=follow)
        kind = "d" if os.path.stat.S_ISDIR(st.st_mode) else "f" if os.path.stat.S_ISREG(st.st_mode) else "o"
        return "%d %d %s" % (st.st_dev, st.st_ino, kind)

    def stats(self, paths, cd=None):
        queries = [("cd", cd)] if cd else []
        for _ in range(2):
            for p in paths:
                queries += [("stat", "F", p), ("stat", "L", p)]
        return queries

    def test_dotdot_answers_like_the_full_walk_and_the_kernel(self):
        paths = [p for p, _ in DOTDOT]
        own = paths.index("/proc/self/fd/..")
        new = self.ask(*self.stats(paths))
        old = self.ask(*self.stats(paths), env={"PROOT_FP_DOTDOT": "0"})
        n = len(paths)
        for k in (2 * own, 2 * own + 1, 2 * (n + own), 2 * (n + own) + 1):
            self.assertTrue(new[k].endswith(" d") and old[k].endswith(" d"))
            new[k] = old[k] = "own"
        self.assertEqual(new, old)
        cold, warm = new[:2 * n], new[2 * n:]
        self.assertEqual(cold, warm)
        for k, (p, rel) in enumerate(DOTDOT):
            follow, nofollow = cold[2 * k], cold[2 * k + 1]
            self.assertEqual(follow, nofollow, p)
            if rel is not None:
                self.assertEqual(follow, self.kernel(rel), p)
        expect = {"/fl/..": "E20", "/file/..": "E20", "/missing/..": "E2", "/d/missing/..": "E2",
                  "/dangling/..": "E2", "/deep/x/y/w/..": "E20", "/loop1/..": "SLOW"}
        for k, (p, _) in enumerate(DOTDOT):
            if p in expect:
                self.assertEqual(cold[2 * k], expect[p], p)

    def test_relative_dotdot_from_the_working_directory(self):
        paths = [p for p, _ in RELATIVE]
        new = self.ask(*self.stats(paths, cd=self.root / "d"))
        old = self.ask(*self.stats(paths, cd=self.root / "d"), env={"PROOT_FP_DOTDOT": "0"})
        self.assertEqual(new, old)
        n = len(paths)
        self.assertEqual(new[:2 * n], new[2 * n:])
        for k, (p, rel) in enumerate(RELATIVE):
            self.assertEqual(new[2 * k], self.kernel(rel), p)

    def test_a_warm_dotdot_takes_two_calls(self):
        out = self.ask(
            ("stat", "F", "/deep/x/y/z/.."), ("count",),
            ("stat", "F", "/deep/x/y/z/.."), ("count",),
            ("stat", "F", "/deep/x/y/z/../.."), ("count",),
            ("stat", "L", "/d/e/f"), ("count",),
            ("stat", "F", "/d/e/h/.."), ("count",),
            ("stat", "L", "/d/g/none"), ("count",),
            ("stat", "F", "/d/g/.."), ("count",),
            ("dotdot", 0),
            ("stat", "F", "/deep/x/y/z/.."), ("count",),
        )
        calls = [int(line.split()[0]) for line in out[1::2]]
        self.assertGreaterEqual(calls[0], 5)
        self.assertLessEqual(calls[1], 2)
        self.assertLessEqual(calls[2], 2)
        self.assertLessEqual(calls[4], 2)
        self.assertLessEqual(calls[6], 2)
        self.assertGreaterEqual(calls[7], 5)
        self.assertEqual(out[0], self.kernel("deep/x/y"))
        self.assertEqual(out[2], out[0])
        self.assertEqual(out[4], self.kernel("deep/x"))
        self.assertEqual(out[8], self.kernel("d/e"))
        self.assertEqual(out[10], "E2")
        self.assertEqual(out[12], self.kernel("d"))
        self.assertEqual(out[14], out[0])

    def test_changes_made_behind_the_cache_are_seen(self):
        r = self.root
        out = self.ask(
            ("mkdir", r / "s/v"),
            ("stat", "F", "/s/v/.."), ("stat", "F", "/s/v/.."), ("ref", "F", r / "s"),
            ("rmdir", r / "s/v"),
            ("stat", "F", "/s/v/.."),
            ("mkdir", r / "s/v"),
            ("stat", "F", "/s/v/.."),
            ("rename", r / "s/v", r / "s/v2"),
            ("stat", "F", "/s/v/.."),
            ("rename", r / "s/v2", r / "s/v"),
            ("stat", "F", "/s/v/.."),
            ("rmdir", r / "s/v"), ("symlink", "other/q", r / "s/v"),
            ("stat", "F", "/s/v/.."), ("ref", "F", r / "s/other"),
            ("unlink", r / "s/v"), ("mkdir", r / "s/v"),
            ("stat", "F", "/s/v/.."),
            ("rmdir", r / "s/v"), ("touch", r / "s/v"),
            ("stat", "F", "/s/v/.."),
            ("stat", "F", "/s/lnk/.."), ("stat", "F", "/s/lnk/.."), ("ref", "F", r / "s/t"),
            ("unlink", r / "s/lnk"), ("symlink", "other/q", r / "s/lnk"),
            ("stat", "F", "/s/lnk/.."),
            ("unlink", r / "s/lnk"), ("symlink", "t/q", r / "s/lnk"),
            ("unlink", r / "s/v"),
            ("symlink", "t/q", r / "s/lnk2"),
            ("stat", "F", "/s/lnk2/none"), ("stat", "F", "/s/lnk2/.."),
            ("unlink", r / "s/lnk2"), ("symlink", "other/q", r / "s/lnk2"),
            ("stat", "F", "/s/lnk2/.."),
            ("unlink", r / "s/lnk2"),
        )
        s, other, t = out[2], out[8], out[13]
        self.assertEqual(out[:2], [s, s])
        self.assertEqual(out[3:6], ["E2", s, "E2"])
        self.assertEqual(out[6], s)
        self.assertEqual(out[7], other)
        self.assertNotEqual(other, s)
        self.assertEqual(out[9:11], [s, "E20"])
        self.assertEqual(out[11:14], [t, t, t])
        self.assertEqual(out[14], other)
        self.assertEqual(out[15:18], ["E2", t, other])

    def test_an_entry_built_on_a_cached_parent_expires_with_it(self):
        r = self.root
        for d in ["rp/v1/a/b/c", "rp/v2/a/b/c"]:
            (r / d).mkdir(parents=True)
        for f in ["rp/v1/a/f", "rp/v2/a/f"]:
            (r / f).touch()
        os.symlink("v1", r / "rp/cur")
        try:
            queries = [
                ("ttl", 300),
                ("stat", "L", "/rp/cur/zz"),
                ("unlink", r / "rp/cur"), ("symlink", "v2", r / "rp/cur"),
                ("nap", 150),
                ("stat", "F", "/rp/cur/a/.."),
                ("nap", 250),
                ("stat", "F", "/rp/cur/a/.."),
                ("stat", "L", "/rp/cur/a/f"),
                ("stat", "F", "/rp/cur/a/b/.."),
                ("nap", 150),
                ("stat", "F", "/rp/cur/a/b/c/.."),
            ]
            new = self.ask(*queries)
            os.unlink(r / "rp/cur")
            os.symlink("v1", r / "rp/cur")
            old = self.ask(*queries, env={"PROOT_FP_DOTDOT": "0"})
            want = [self.kernel("rp/v2"), self.kernel("rp/v2/a/f"), self.kernel("rp/v2/a"), self.kernel("rp/v2/a/b")]
            self.assertIn(new[1], [self.kernel("rp/v1"), self.kernel("rp/v2")])
            self.assertEqual(new[2:], want)
            self.assertEqual(old[2:], want)
        finally:
            shutil.rmtree(r / "rp")

    def test_symbolic_links_are_counted_over_the_whole_path(self):
        r = self.root
        (r / "lk/a/b/c/d").mkdir(parents=True)
        for where, name, target in [("lk", "k", "a"), ("lk/a", "m", "b"), ("lk/a/b", "n", "c"), ("lk/a/b/c", "o", "d")]:
            os.symlink(target, r / where / (name + "0"))
            for k in range(1, 16):
                os.symlink("%s%d" % (name, k - 1), r / where / ("%s%d" % (name, k)))
        try:
            looping = ["/lk/k15/m15/../m15", "/lk/k15/m15/n15/..", "/lk/k15/m15/n15", "/lk/k15/m15/n15/o15/..",
                       "/lk/k15/m15/n15/o15/zz"]
            for p in looping:
                with self.assertRaises(OSError) as e:
                    os.stat(str(r) + p)
                self.assertEqual(e.exception.errno, 40, p)
            queries = []
            for _ in range(2):
                queries += [("stat", "F", "/lk/k15/m15/..")] + [("stat", "F", p) for p in looping]
            out = self.ask(*queries)
            n = 1 + len(looping)
            self.assertEqual(out[:n], out[n:])
            self.assertEqual(out[0], self.kernel("lk/a"))
            self.assertEqual(out[1:n], ["SLOW"] * len(looping))
        finally:
            shutil.rmtree(r / "lk")

    def test_a_raw_getxattr_reads_the_name_through_the_dispatcher(self):
        out = self.ask(("rawx", "F", "/x/old2", "user.q"), ("rawx", "F", "/x/old2", "bad"),
                       ("rawx", "L", "/x/old2", "bad"), ("rawx", "F", "/x/old2", "edge"),
                       ("rawx", "F", "/x/old2", ""), ("rawx", "F", "/x/missing", "bad"))
        if out[0] == "NOSVC":
            self.skipTest("the raw-syscall dispatcher is built only for aarch64")
        try:
            os.getxattr(self.root / "x/old2", "user.q")
            first = None
        except OSError as e:
            first = "E%d" % e.errno
        self.assertEqual(out[0], first)
        self.assertEqual(out[1:4], ["SLOW"] * 3)
        self.assertEqual(out[4], "E34")
        self.assertEqual(out[5], "SLOW")

    def test_random_paths_match_the_full_walk(self):
        names = ["d", "e", "g", "h", "f", "file", "rel", "chain", "abs", "fl", "up", "dangling", "missing",
                 "s", "lnk", "t", "q", "other", "p", "mnt", "sd", "sub", "out", "deep", "x", "y", "z", "w",
                 "..", "..", "..", ".", ""]
        rng = random.Random(1115)
        paths = ["/" + "/".join(rng.choice(names) for _ in range(rng.randint(1, 7))) for _ in range(400)]
        queries = []
        for _ in range(2):
            for p in paths:
                queries += [("stat", "F", p), ("stat", "L", p)]
        new = self.ask(*queries)
        old = self.ask(*queries, env={"PROOT_FP_DOTDOT": "0"})
        self.assertEqual(len(new), 4 * len(paths))
        for k, (a, b) in enumerate(zip(new, old)):
            self.assertEqual(a, b, paths[(k // 2) % len(paths)])

    def test_a_missing_user_attribute_is_remembered_until_the_inode_changes(self):
        probe = self.root / "x/probe"
        probe.touch()
        try:
            os.setxattr(probe, "user.probe", b"1")
        except OSError:
            self.skipTest("no user xattrs here")
        finally:
            probe.unlink()
        wait = self.xattr_since + 3.2 - time.monotonic()
        if wait > 0:
            time.sleep(wait)
        old = self.root / "x/old"
        out = self.ask(
            ("getx", "F", "/x/old", "user.a"), ("count",),
            ("getx", "F", "/x/old", "user.a"), ("count",),
            ("getx", "F", "/x/old", "user.b"), ("count",),
            ("getx", "L", "/x/link", "user.a"), ("count",),
            ("getx", "F", "/x/link", "user.a"), ("count",),
            ("setx", old, "user.a", "v1"),
            ("getx", "F", "/x/old", "user.a"), ("count",),
            ("getx", "F", "/x/old", "user.a"), ("count",),
            ("remx", old, "user.a"),
            ("getx", "F", "/x/old", "user.a"), ("count",),
            ("getx", "F", "/x/old", "user.a"), ("count",),
            ("touch", self.root / "x/new"),
            ("getx", "F", "/x/new", "user.a"), ("count",),
            ("getx", "F", "/x/new", "user.a"), ("count",),
            ("getx", "F", "/x/old2", "trusted.dd"), ("count",),
            ("getx", "F", "/x/old2", "trusted.dd"), ("count",),
            ("getx", "F", "/x/missing", "user.a"), ("count",),
            ("ttl", 50),
            ("getx", "F", "/x/old2", "user.z"), ("count",),
            ("getx", "F", "/x/old2", "user.z"), ("count",),
            ("nap", 120),
            ("getx", "F", "/x/old2", "user.z"), ("count",),
            ("xattr", 0),
            ("getx", "F", "/x/old2", "user.y"), ("count",),
            ("getx", "F", "/x/old2", "user.y"), ("count",),
        )
        answers, xattr_calls = out[0::2], [int(line.split()[1]) for line in out[1::2]]
        self.assertEqual(answers[:5], ["E61"] * 5)
        self.assertEqual(xattr_calls[:5], [1, 0, 1, 1, 0])
        self.assertEqual(answers[5:7], ["2", "2"])
        self.assertEqual(xattr_calls[5:7], [1, 1])
        self.assertEqual(answers[7:11], ["E61"] * 4)
        self.assertEqual(xattr_calls[7:11], [1, 1, 1, 1])
        self.assertEqual(xattr_calls[11:13], [1, 1])
        self.assertEqual((answers[13], xattr_calls[13]), ("E2", 0))
        self.assertEqual(answers[14:19], ["E61"] * 5)
        self.assertEqual(xattr_calls[14:19], [1, 0, 1, 1, 1])
        os.unlink(self.root / "x/new")
        off = self.ask(("getx", "F", "/x/old2", "user.a"), ("count",), ("getx", "F", "/x/old2", "user.a"), ("count",),
                       env={"PROOT_FP_XATTR": "0"})
        self.assertEqual([int(line.split()[1]) for line in off[1::2]], [1, 1])


if __name__ == "__main__":
    unittest.main()
