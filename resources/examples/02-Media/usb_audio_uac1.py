"""Run this example from the IDE with the UAC1 candidate firmware.

The pet must release its audio streams before the test. Change MODE to
'record' or 'play' to exercise each direction. The default only inspects.
'record' reports capture level, never saves the microphone recording.
"""
import math
import time
from media.usb_audio import capabilities, Microphone, Output

MODE = 'inspect'
FRAMES = 320  # 20 ms at the application sample rate
DURATION_MS = 3000


def capture():
    mic = Microphone(FRAMES)
    samples = peak = square = 0
    start = time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), start) < DURATION_MS:
            data = mic.read_frame()
            if data is None:
                time.sleep_ms(2)
                continue
            for i in range(0, len(data), 2):
                sample = data[i] | (data[i + 1] << 8)
                if sample & 32768:
                    sample -= 65536
                peak = max(peak, abs(sample))
                square += sample * sample
                samples += 1
        print('capture', mic.info(), 'samples', samples, 'peak', peak,
              'rms', math.sqrt(square / max(1, samples)))
        if not samples:
            raise OSError('no capture samples')
    finally:
        mic.close()


def playback():
    out = Output(volume=50, frames=FRAMES)
    block = bytearray(FRAMES * 2)
    for i in range(FRAMES):
        value = int(4200 * math.sin(2 * math.pi * 750 * i / 16000)) & 65535
        block[i * 2] = value & 255
        block[i * 2 + 1] = value >> 8
    start = time.ticks_ms()
    blocks = 0
    try:
        while blocks < DURATION_MS // 20:
            if time.ticks_diff(time.ticks_ms(), start) > DURATION_MS + 3000:
                raise OSError('USB output stalled')
            if out.write(block):
                blocks += 1
            else:
                time.sleep_ms(2)
        while not out.drained():
            if time.ticks_diff(time.ticks_ms(), start) > DURATION_MS + 4000:
                raise OSError('USB output did not drain')
            time.sleep_ms(2)
        print('playback', out.info(), 'accepted 20 ms blocks', blocks)
    finally:
        out.close()


print('UAC1', capabilities())
if MODE == 'record':
    capture()
elif MODE == 'play':
    playback()
elif MODE != 'inspect':
    raise ValueError('MODE must be inspect, record or play')
