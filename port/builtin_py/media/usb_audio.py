"""Nonblocking 16 kHz mono PCM16 adapters for the optional USB Audio Class 1 host.

capabilities() is non-claiming. Capture and playback are explicitly owned,
half-duplex streams. An unplugged/unavailable device raises OSError.
"""
import _uac1

CAP_FIELDS = ('abi', 'connected', 'vid', 'pid', 'generation', 'capture_channels',
              'playback_channels', 'rate')
INFO_FIELDS = ('abi', 'connected', 'rate', 'channels', 'sample_bits', 'buffered_bytes',
               'inflight_bytes', 'xruns', 'vid', 'pid', 'generation', 'io_errors')


def capabilities():
    return dict(zip(CAP_FIELDS, _uac1.capabilities()))


def _open(direction):
    dev = _uac1.PCM(direction)
    try:
        info = dev.info()
        if (info[0] != 1 or not info[1] or info[2] != 48000 or
                info[3] not in (1, 2) or info[4] != 16):
            raise OSError('unsupported USB PCM profile')
        return dev, info
    except BaseException:
        dev.close()
        raise


def _frame_bytes(frames):
    if type(frames) is not int or not 1 <= frames <= 2560:
        raise ValueError('frames must be between 1 and 2560 at 16 kHz')
    return frames * 2


class Microphone:
    """read_frame returns one complete application block or None when not ready."""
    def __init__(self, frames=2560):
        self.frame_bytes = _frame_bytes(frames)
        self.dev, info = _open('capture')
        try:
            self.generation, self.xruns, self.io_errors = info[10], info[7], info[11]
            self.channels = info[3]
            self.raw = memoryview(bytearray(self.frame_bytes * 3 * self.channels))
            self.out = memoryview(bytearray(self.frame_bytes))
            self.offset = 0
        except BaseException:
            self.dev.close()
            raise

    def read_frame(self):
        if self.dev is None:
            raise OSError('USB microphone is closed')
        remaining = self.frame_bytes - self.offset
        n = self.dev.readinto(self.raw[:remaining * 3 * self.channels])
        if n:
            self.offset += self.dev.decode_into(self.raw[:n], self.out[self.offset:])
        if self.offset != self.frame_bytes:
            return None
        # Don't splice PCM from before a ring overrun or unplug into a sentence.
        info = self.dev.info()
        if (not info[1] or info[10] != self.generation or info[7] != self.xruns
                or info[11] != self.io_errors):
            raise OSError('USB capture discontinuity')
        self.offset = 0
        return bytes(self.out)

    def info(self):
        return dict(zip(INFO_FIELDS, self.dev.info()))

    def close(self):
        if self.dev is not None:
            self.dev.close()
            self.dev = None


class Output:
    """write accepts complete blocks. A False return requires a retry of the same bytes."""
    def __init__(self, volume=60, frames=2560):
        self.frame_bytes = _frame_bytes(frames)
        if type(volume) is not int or not 0 <= volume <= 100:
            raise ValueError('volume must be 0..100')
        self.volume = volume
        self.closed = False
        self.dev = None
        self.pending = None
        self._acquire()

    def _acquire(self):
        if self.closed:
            raise OSError('USB output is closed')
        if self.dev is not None:
            return
        dev, info = _open('playback')
        try:
            self.native = memoryview(bytearray(self.frame_bytes * 3 * info[3]))
            self.generation, self.io_errors = info[10], info[11]
            self.pos = self.end = 0
            self.pending = None
            self.dev = dev
        except BaseException:
            dev.close()
            raise

    def set_volume(self, volume):
        if type(volume) is not int or not 0 <= volume <= 100:
            raise ValueError('volume must be 0..100')
        if self.closed:
            raise OSError('USB output is closed')
        if volume == 0 and self.dev is not None:
            # Muting an interrupted reply must also silence queued native PCM.
            try:
                self.dev.drop()
            finally:
                self.dev.close()
                self.dev = None
                self.pending = None
        self.volume = volume

    def write(self, block):
        if len(block) != self.frame_bytes:
            raise ValueError('expected one complete 16 kHz mono PCM16 block')
        self._acquire()
        if self.pending is None:
            self.pending = bytes(block)
            self.pos = 0
            self.end = self.dev.encode_into(self.pending, self.native, self.volume)
        elif self.pending != block:
            raise ValueError('retry the pending PCM block')
        self.pos += self.dev.write(self.native[self.pos:self.end])
        if self.pos != self.end:
            return False
        self.pending = None
        return True

    def drained(self):
        if self.dev is None:
            return self.pending is None
        v = self.dev.info()
        if not v[1] or v[10] != self.generation or v[11] != self.io_errors:
            raise OSError('USB playback discontinuity')
        return self.pending is None and v[5] == 0 and v[6] == 0

    def info(self):
        return dict(zip(INFO_FIELDS, self.dev.info()))

    def close(self):
        if self.dev is not None:
            self.dev.close()
            self.dev = None
        self.pending = None
        self.closed = True
