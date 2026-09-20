"""Check programming command construction without contacting hardware."""
import importlib.util
import contextlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("flash", Path(__file__).resolve().parents[2] / "tools/flash.py")
flash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flash)


class Programming(unittest.TestCase):
    def test_plan_without_programming_tools(self):
        with tempfile.TemporaryDirectory() as directory:
            images = [Path(directory) / name for name in ('m4.elf', 'm7.elf')]
            for image in images:
                image.touch()
            output = io.StringIO()
            with patch('sys.argv', ['flash.py', '--backend', 'plan', '--family', 'stm32h7',
                                    *map(str, images)]), \
                    patch.object(flash.shutil, 'which', return_value=None), \
                    patch.object(flash.subprocess, 'run') as run, \
                    contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
                flash.main()
            commands = output.getvalue().splitlines()
            self.assertEqual(len(commands), 2)
            self.assertTrue(commands[0].startswith('STM32_Programmer_CLI '))
            self.assertTrue(commands[1].startswith('openocd '))
            for command in commands:
                for image in images:
                    self.assertIn(str(image), command)
            run.assert_not_called()

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


def test_boot_layout_reserves_flash_otp_inside_bank_b_placeholder(tmp_path, monkeypatch):
    layout_spec = importlib.util.spec_from_file_location('boot_layout', Path(__file__).resolve().parents[2] / 'tools/boot_layout.py')
    boot_layout = importlib.util.module_from_spec(layout_spec)
    layout_spec.loader.exec_module(boot_layout)
    import json
    import sys

    source = tmp_path / 'source.ld'
    source.write_text('MEMORY { FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 2048K }\n')
    monkeypatch.setattr(boot_layout, 'flash_size', lambda _: 18328)
    monkeypatch.setattr(sys, 'argv', ['boot_layout', '--probe', str(tmp_path / 'probe.elf'),
                                     '--source-linker', str(source), '--directory', str(tmp_path)])
    boot_layout.main()
    layout = json.loads((tmp_path / 'layout.json').read_text())
    assert layout['otp_emulator_base'] == 0x08100000
    assert layout['otp_emulator_size'] == 8192
    assert layout['otp_emulator_base'] + layout['otp_emulator_size'] <= layout['metadata'][1]
    assert layout['metadata'] == [0x08008000, 0x08108000]
    assert layout['slots'] == [0x0800a000, 0x0810a000]
    assert 'kOtpEmulatorBase' in (tmp_path / 'layout.h').read_text()
    assert 'ORIGIN = 0x0810a000' in (tmp_path / 'slot_b.ld').read_text()
