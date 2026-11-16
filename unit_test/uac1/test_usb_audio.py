"""Exercise partial native transfers, real PCM wire geometry, and disconnect recovery."""
import importlib.util
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch

API = types.SimpleNamespace()
spec = importlib.util.spec_from_file_location('usb_audio',
    Path(__file__).resolve().parents[2] / 'port/builtin_py/media/usb_audio.py')
audio = importlib.util.module_from_spec(spec)
with patch.dict(sys.modules, {'_uac1': API}):
    spec.loader.exec_module(audio)


class PCM:
    def __init__(self, direction):
        self.channels = 2
        self.direction = direction
        self.pending, self.queued = bytearray(), bytearray()
        self.closed = self.dropped = False
        self.converts = self.failures = self.xruns = self.inflight = 0
        self.block_size = 12
        self.input_frames = 0

    def info(self):
        return (1, 1, 48000, self.channels, 16, len(self.queued), self.inflight,
                self.xruns, 0x4c4a, 0x4155, 1, self.failures)

    def readinto(self, buffer):
        n = min(len(buffer), len(self.pending), self.block_size)
        buffer[:n] = self.pending[:n]
        del self.pending[:n]
        return n

    def decode_into(self, src, dst):
        self.input_frames += len(src) // 4
        n = self.input_frames // 3 * 2
        self.input_frames %= 3
        dst[:n] = bytes(n)
        return n

    def encode_into(self, src, dst, volume):
        self.converts += 1
        n = len(src) * self.channels * 3
        dst[:n] = bytes(range(n))
        return n

    def write(self, src):
        n = min(len(src), self.block_size)
        self.queued += bytes(src[:n])
        return n

    def drop(self):
        self.dropped = True
        self.queued.clear()

    def close(self):
        self.closed = True


class USBAdapterTests(unittest.TestCase):
    def setUp(self):
        self.streams = []
        def factory(direction):
            obj = PCM(direction)
            self.streams.append(obj)
            return obj
        API.PCM = factory
        API.capabilities = lambda: (1, 1, 0x4c4a, 0x4155, 1, 2, 2, 48000)

    def test_caps_do_not_acquire_an_audio_owner(self):
        self.assertEqual(audio.capabilities()['rate'], 48000)
        self.assertFalse(self.streams)

    def test_fragmented_capture_preserves_boundaries_and_flags_native_loss(self):
        mic = audio.Microphone(frames=5)
        dev = self.streams[0]
        dev.block_size = 4
        self.assertIsNone(mic.read_frame())
        dev.pending.extend(bytes(5 * 3 * 4))
        for _ in range(14):
            self.assertIsNone(mic.read_frame())
        self.assertEqual(mic.read_frame(), bytes(10))
        dev.pending.extend(bytes(5 * 3 * 4))
        dev.block_size = len(dev.pending)
        dev.xruns = 1
        with self.assertRaises(OSError):
            mic.read_frame()
        mic.close()
        self.assertTrue(dev.closed)

    def test_output_partial_write_does_not_convert_or_replay_prefix(self):
        out = audio.Output(frames=4)
        dev = self.streams[0]
        src = bytes(8)
        self.assertFalse(out.write(src))
        with self.assertRaises(ValueError):
            out.write(bytes([1]*8))
        for _ in range(2):
            self.assertFalse(out.write(src))
        self.assertTrue(out.write(src))
        self.assertEqual(dev.converts, 1)
        self.assertEqual(dev.queued, bytearray(range(48)))
        self.assertFalse(out.drained())
        dev.queued.clear()
        dev.inflight = 192
        self.assertFalse(out.drained())
        dev.inflight = 0
        self.assertTrue(out.drained())
        dev.failures = 1
        with self.assertRaises(OSError):
            out.drained()
        out.close()

    def test_muting_discards_partial_pcm_and_reopens_only_for_new_audio(self):
        out = audio.Output(frames=4)
        dev = self.streams[0]
        self.assertFalse(out.write(bytes(8)))
        out.set_volume(0)
        self.assertTrue(dev.dropped and dev.closed)
        self.assertEqual(len(self.streams), 1)
        out.set_volume(60)
        self.assertFalse(out.write(bytes(8)))
        self.assertIsNot(self.streams[-1], dev)
        out.close()


if __name__ == '__main__':
    unittest.main()
