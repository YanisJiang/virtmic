"""Low-latency rough pitch shift via delay-line read-rate change."""

from __future__ import annotations

import array
import math


class PitchShifter:
    """Stateful shifter: write 1x, read at 2^(semitones/12) through a ring."""

    def __init__(self, capacity: int = 480 * 8) -> None:
        # ~80 ms @ 48 kHz — enough headroom for ±12 semitones without instant wrap.
        self._cap = max(capacity, 480 * 4)
        self._buf = array.array("f", [0.0] * self._cap)
        self._write = 0
        self._read = 0.0
        self._primed = False
        self._latency = self._cap // 2

    def reset(self) -> None:
        for i in range(self._cap):
            self._buf[i] = 0.0
        self._write = 0
        self._read = 0.0
        self._primed = False

    def process(self, frame: array.array, semitones: int) -> array.array:
        n = len(frame)
        if n == 0:
            return array.array("f")

        for i in range(n):
            self._buf[self._write % self._cap] = frame[i]
            self._write += 1

        if not self._primed:
            if self._write < self._latency:
                return array.array("f", [0.0] * n)
            self._read = float(self._write - self._latency)
            self._primed = True

        if semitones == 0:
            # 1:1 from the just-written region (same as passthrough after prime).
            return array.array("f", frame)

        ratio = math.pow(2.0, float(semitones) / 12.0)
        out = array.array("f", [0.0] * n)
        for i in range(n):
            out[i] = self._sample_at(self._read)
            self._read += ratio

        # Keep the read head roughly latency samples behind the write head.
        target = float(self._write - self._latency)
        # If we drifted too far (underrun/overrun), snap back with a soft reset.
        drift = self._read - target
        if drift > self._latency or drift < -self._latency:
            self._read = target

        return out

    def _sample_at(self, pos: float) -> float:
        # pos may be large; wrap into buffer.
        p = pos % self._cap
        if p < 0:
            p += self._cap
        i0 = int(p) % self._cap
        i1 = (i0 + 1) % self._cap
        frac = p - int(p)
        return self._buf[i0] * (1.0 - frac) + self._buf[i1] * frac


# Module-level shifter for simple call sites / tests that process multiple frames.
_default = PitchShifter()


def shift_semitones(frame: array.array, semitones: int) -> array.array:
    """Process one frame with the module-level shifter (stateful across calls)."""
    return _default.process(frame, semitones)


def reset_default_shifter() -> None:
    _default.reset()
