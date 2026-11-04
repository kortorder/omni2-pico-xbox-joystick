#!/usr/bin/env python3
"""Exercise the private build-input boundary using synthetic bytes only."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('private_peer', Path(__file__).resolve().parents[1] / 'cmake/private_peer.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PrivatePinTest(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.base = Path(self.scratch.name)
        self.repo = self.base / 'source'
        self.repo.mkdir()
        self.pin = self.base / 'pin.bin'
        self.pin.write_bytes(bytes(range(1, 7)))
        self.pin.chmod(0o600)
        self.out = self.base / 'private-build/private-peer/omni_approved_peer.hpp'

    def tearDown(self):
        self.scratch.cleanup()

    def test_generated_header_is_private_and_matches_input(self):
        module.generate(self.pin, self.out, self.repo)
        self.assertIn('OMNI_PINNED_MOUSE2_BYTES 1,2,3,4,5,6', self.out.read_text())
        self.assertEqual(self.out.stat().st_mode & 0o777, 0o600)

    def test_rejects_exposed_or_symlink_inputs(self):
        self.pin.chmod(0o640)
        with self.assertRaises(ValueError):
            module.generate(self.pin, self.out, self.repo)
        link = self.base / 'link'
        link.symlink_to(self.pin)
        with self.assertRaises(OSError):
            module.generate(link, self.out, self.repo)
        self.assertFalse(self.out.exists())

    def test_rejects_text_placeholders_and_wrong_lengths(self):
        for invalid in [b'', b'abcdefg', bytes(6), bytes([255])*6, b'01:02:03:04:05:06']:
            self.pin.write_bytes(invalid)
            with self.assertRaises(ValueError):
                module.generate(self.pin, self.out, self.repo)
        self.assertFalse(self.out.exists())

    def test_refuses_pin_and_header_in_repository(self):
        path = self.repo / 'pin.bin'
        path.write_bytes(self.pin.read_bytes())
        path.chmod(0o600)
        with self.assertRaises(ValueError):
            module.generate(path, self.out, self.repo)
        with self.assertRaises(ValueError):
            module.generate(self.pin, self.repo / 'generated.hpp', self.repo)
        self.assertFalse(self.out.exists())


if __name__ == '__main__':
    unittest.main()
