"""Exercise decoder failure and explicit frame holds with real FFmpeg processes."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import render_onboarding as renderer


class SourceDecoderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.root = Path(cls.directory.name)
        cls.complete = cls.root / 'complete.mp4'
        subprocess.run([
            'ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
            'testsrc=size=40x24:rate=10', '-t', '2', '-c:v', 'libx264rgb',
            '-crf', '0', '-g', '1', '-movflags', '+faststart', str(cls.complete),
        ], check=True)
        contents = cls.complete.read_bytes()
        cls.truncated = cls.root / 'truncated.mp4'
        cls.truncated.write_bytes(contents[:len(contents) * 3 // 4])

    def source(self, path, hold_last=False):
        source = renderer.Source(
            {'path': str(path), 'hold_last': hold_last},
            'browser', self.root, 10, False)
        self.addCleanup(source.close)
        return source

    def test_complete_sequence(self):
        source = self.source(self.complete)
        frames = [source.frame(index / 10).tobytes() for index in range(20)]
        self.assertEqual(len(frames), 20)
        self.assertNotEqual(frames[0], frames[-1])
        self.assertFalse(source.held)

    def test_truncated_capture_never_becomes_a_hold(self):
        for hold_last in (False, True):
            with self.subTest(hold_last=hold_last):
                source = self.source(self.truncated, hold_last)
                self.assertEqual(source.duration, 2)
                self.assertIsNotNone(source.frame(0))
                with self.assertRaises(ValueError):
                    for index in range(1, 20):
                        source.frame(index / 10)

    def test_clean_early_decoder_exit_is_an_error(self):
        source = self.source(self.complete)
        popen = subprocess.Popen

        def one_frame_decoder(command, **kwargs):
            return popen([
                'ffmpeg', '-v', 'error', '-i', str(self.complete),
                '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1',
            ], **kwargs)

        with patch.object(renderer.subprocess, 'Popen', one_frame_decoder):
            source.frame(0)
            with self.assertRaises(ValueError):
                source.frame(0.1)

    def test_source_overrun_requires_explicit_hold(self):
        source = self.source(self.complete)
        source.frame(1.9)
        with self.assertRaises(ValueError):
            source.frame(2)

    def test_explicit_hold_preserves_final_frame(self):
        source = self.source(self.complete, hold_last=True)
        final_frame = source.frame(1.9).tobytes()
        self.assertEqual(source.frame(3).tobytes(), final_frame)
        self.assertTrue(source.held)


if __name__ == '__main__':
    unittest.main()
