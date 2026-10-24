import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

SESSION = Path(__file__).resolve().parents[1] / "linuxfs/overlay/usr/local/bin/droiddeck-session"
GLIBC = "glibc-2.43+r22+g8362e8ce10b2-2"
LOCALES = "en_US.UTF-8 zh_CN.UTF-8 zh_TW.UTF-8 ja_JP.UTF-8 ko_KR.UTF-8"


def locale_function(root):
    match = re.search(r"^prepare_locale\(\) \{\n.*?^\}\n", SESSION.read_text(), re.M | re.S)
    return re.sub(r"(?<![\w.])/(usr|var)/", "%s/\\1/" % root, match.group(0))


class SessionLocaleTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root, True)
        (self.root / "usr/lib/locale/C.utf8").mkdir(parents=True)
        (self.root / "var/lib/pacman/local" / GLIBC).mkdir(parents=True)
        sources = self.root / "usr/share/i18n/locales"
        sources.mkdir(parents=True)
        for name in ["en_US", "zh_CN", "zh_TW", "ja_JP", "ko_KR"]:
            (sources / name).write_text("source")
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.calls = self.root / "calls"
        localedef = self.bin / "localedef"
        localedef.write_text('#!/bin/bash\necho "$* LINK_RENAME=$DROIDDECK_LINK_RENAME" >> %s\n'
                             ': >> %s/usr/lib/locale/locale-archive\n'
                             '[ "${@: -1}" != "$FAIL" ]\n' % (self.calls, self.root))
        localedef.chmod(0o755)
        iconvconfig = self.bin / "iconvconfig"
        iconvconfig.write_text("#!/bin/sh\n")
        iconvconfig.chmod(0o755)

    def prepare(self, **env):
        self.calls.unlink(missing_ok=True)
        env = dict(os.environ, PATH="%s:%s" % (self.bin, os.environ["PATH"]), **{"FAIL": "", **env})
        out = subprocess.run(["bash", "-c", locale_function(self.root) + "prepare_locale"], env=env,
                             capture_output=True, text=True, timeout=30, check=True).stdout
        calls = self.calls.read_text().splitlines() if self.calls.exists() else []
        return out, calls

    def stamp(self):
        return (self.root / "usr/lib/locale/.droiddeck-archive").read_text().strip()

    def test_an_archive_with_only_c_is_rebuilt_once_with_steam_and_cjk_locales(self):
        (self.root / "usr/lib/locale/locale-archive").write_text("")
        (self.root / "usr/lib/locale/.droiddeck-archive").write_text(GLIBC + "\n")
        os.utime(self.root / "usr/lib/locale/locale-archive", (1, 1))
        out, calls = self.prepare()
        self.assertEqual([
            "--add-to-archive --replace %s/usr/lib/locale/C.utf8 LINK_RENAME=1" % self.root,
            "--replace -i en_US -f UTF-8 en_US.UTF-8 LINK_RENAME=1",
            "--replace -i zh_CN -f UTF-8 zh_CN.UTF-8 LINK_RENAME=1",
            "--replace -i zh_TW -f UTF-8 zh_TW.UTF-8 LINK_RENAME=1",
            "--replace -i ja_JP -f UTF-8 ja_JP.UTF-8 LINK_RENAME=1",
            "--replace -i ko_KR -f UTF-8 ko_KR.UTF-8 LINK_RENAME=1",
        ], calls)
        self.assertEqual(GLIBC + " " + LOCALES, self.stamp())
        self.assertIn("== locale archive built for", out)
        os.utime(self.root / "usr/lib/locale/locale-archive", (1, 1))
        self.assertEqual(("", []), self.prepare())

    def test_the_app_language_does_not_change_what_is_built(self):
        _, calls = self.prepare(BL_STEAM_LANGUAGE="japanese")
        self.assertEqual(6, len(calls))
        self.assertEqual(("", []), self.prepare(BL_STEAM_LANGUAGE="schinese"))

    def test_a_locale_that_fails_is_named_left_out_of_the_stamp_and_tried_again(self):
        out, calls = self.prepare(FAIL="zh_CN.UTF-8")
        self.assertEqual(6, len(calls))
        self.assertIn("(could not build: zh_CN.UTF-8, trying again next start)", out)
        self.assertEqual(GLIBC + " en_US.UTF-8 zh_TW.UTF-8 ja_JP.UTF-8 ko_KR.UTF-8", self.stamp())
        os.utime(self.root / "usr/lib/locale/locale-archive", (1, 1))
        out, calls = self.prepare()
        self.assertIn("--replace -i zh_CN -f UTF-8 zh_CN.UTF-8 LINK_RENAME=1", calls)
        self.assertNotIn("could not build", out)
        self.assertEqual(GLIBC + " " + LOCALES, self.stamp())
        os.utime(self.root / "usr/lib/locale/locale-archive", (1, 1))
        self.assertEqual(("", []), self.prepare())

    def test_a_locale_without_sources_is_not_compiled_and_not_stamped(self):
        (self.root / "usr/share/i18n/locales/ko_KR").unlink()
        out, calls = self.prepare()
        self.assertNotIn("ko_KR", "\n".join(calls))
        self.assertIn("(could not build: ko_KR.UTF-8", out)
        self.assertNotIn("ko_KR", self.stamp())


if __name__ == "__main__":
    unittest.main()
