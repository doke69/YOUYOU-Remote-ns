"""Supply-chain regression: missing/mutated sources and compiled-file coverage."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import fetch_rbe_sources as sources


class RbeSourcesTest(unittest.TestCase):
    def test_missing_and_mutated_inputs_are_rejected(self):
        with tempfile.TemporaryDirectory(prefix='rbe-lock-') as raw:
            root = Path(raw)
            expected = b'fixed-public-source'
            lock = {'files_sha256': {'webrtc/test.cc': hashlib.sha256(expected).hexdigest()}}
            with self.assertRaises(ValueError):
                sources.verify_sources(root, lock)
            target = root / 'webrtc/test.cc'
            target.parent.mkdir()
            target.write_bytes(b'changed')
            with self.assertRaises(ValueError):
                sources.verify_sources(root, lock)
            target.write_bytes(expected)
            self.assertEqual(sources.verify_sources(root, lock)['files_verified'], 1)

    def test_every_compiled_upstream_source_and_generator_is_locked(self):
        lock = sources.read_lock()['files_sha256']
        names = [line.strip() for line in (ROOT / 'cmake/webrtc-rbe/sources.cmake').read_text(encoding='utf-8').splitlines()
                 if line.strip().endswith('.cc')]
        self.assertGreater(len(names), 20)
        for name in names + ['experiments/field_trials.py', 'LICENSE', 'PATENTS', 'AUTHORS']:
            self.assertIn('webrtc/' + name, lock)
        self.assertIn('abseil/LICENSE', lock)
        self.assertNotIn('rtc_base/platform_thread_types.cc', names)


if __name__ == '__main__':
    unittest.main()
