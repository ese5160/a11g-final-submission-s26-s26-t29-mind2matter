#!/usr/bin/env python3
from __future__ import annotations

import math
import struct
import sys
import wave
from pathlib import Path


def main() -> int:
    if len(sys.argv) not in {2, 3}:
        print("Usage: generate_test_wav.py OUTPUT.wav [SECONDS]")
        return 1

    output_path = Path(sys.argv[1]).resolve()
    duration_s = float(sys.argv[2]) if len(sys.argv) == 3 else 1.0
    sample_rate = 24000
    frequency_hz = 440.0
    amplitude = 10000
    frame_count = max(1, int(sample_rate * duration_s))

    output_path.parent.mkdir(parents=True, exist_ok=True)

    with wave.open(str(output_path), "wb") as wav_file:
        wav_file.setnchannels(1)
        wav_file.setsampwidth(2)
        wav_file.setframerate(sample_rate)

        frames = bytearray()
        for index in range(frame_count):
            sample = int(amplitude * math.sin((2.0 * math.pi * frequency_hz * index) / sample_rate))
            frames.extend(struct.pack("<h", sample))
        wav_file.writeframes(bytes(frames))

    print(output_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
