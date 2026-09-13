"""Check programming command construction without contacting hardware."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("flash", Path(__file__).resolve().parents[2] / "tools/flash.py")
flash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flash)


class Programming(unittest.TestCase):
    def test_cube_verifies_both_images_before_reset(self):
        images = [Path("m4.elf"), Path("m7.elf")]
        args = flash.command("cubeprogrammer", "cli", "stm32h7", images, "123")
        self.assertEqual(args[-7:], ["-d", "m4.elf", "-v", "-d", "m7.elf", "-v", "-rst"])
        self.assertIn("mode=UR", args)
        self.assertIn("sn=123", args)
        self.assertNotIn("-e", args)

    def test_openocd_h7_enables_both_cores_and_resets_once_after_writes(self):
        args = flash.command("openocd", "ocd", "stm32h7", [Path("m4.elf"), Path("m7.elf")], "")
        self.assertIn("set DUAL_BANK 1; set DUAL_CORE 1", args)
        self.assertIn("target/stm32h7x.cfg", args)
        self.assertEqual(args[-1], "reset run; shutdown")
        self.assertEqual(sum(a.startswith("verify_image ") for a in args), 2)

    def test_h5_and_tcl_quoting(self):
        path = Path('a path/$x[foo]".elf')
        args = flash.command("openocd", "ocd", "stm32h5", [path], "123")
        self.assertIn("target/stm32h5x.cfg", args)
        self.assertFalse(any("DUAL_CORE" in a for a in args))
        self.assertIn('verify_image "a path/\\$x\\[foo\\]\\\".elf"', args)


if __name__ == "__main__":
    unittest.main()
