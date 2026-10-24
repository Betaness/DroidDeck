"""mali-g2/tools/xr-qos.py: the mode values it sends to /dev/xr_fbt."""
import importlib.util
import pathlib
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "mali-g2" / "tools" / "xr-qos.py"


def load():
    spec = importlib.util.spec_from_file_location("xr_qos", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ValueTest(unittest.TestCase):
    def setUp(self):
        self.qos = load()

    def test_named_modes(self):
        self.assertEqual(self.qos.value("vip"), 0x26)
        self.assertEqual(self.qos.value("rt"), 0x19)
        self.assertEqual(self.qos.value("off"), 0)

    def test_level_and_type(self):
        self.assertEqual(self.qos.value("7:1"), 0x17)
        self.assertEqual(self.qos.value("6:2"), 0x26)
        self.assertEqual(self.qos.value("9:1:rtg"), 0x59)
        self.assertEqual(self.qos.value("5:0"), 0x05)

    def test_refused_like_libxqos(self):
        for mode in ("2:0", "6:0", "6:1", "9:0", "11:1", "4:3", "4:0:foo", "4", "x:1", "4:1:rtg:1"):
            with self.assertRaises(ValueError, msg=mode):
                self.qos.value(mode)


if __name__ == "__main__":
    unittest.main()
