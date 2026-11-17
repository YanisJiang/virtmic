"""Low-latency rough pitch shift via rate change + linear resample."""

from __future__ import annotations

import array


def shift_semitones(frame: array.array, semitones: int) -> array.array:
    n = len(frame)
    if semitones == 0 or n == 0:
        return array.array("f", frame)
    ratio = 2.0 ** (semitones / 12.0)
    out = array.array("f", [0.0] * n)
    for i in range(n):
        src = i * ratio
        j = int(src)
        frac = src - j
        if j >= n - 1:
            out[i] = frame[n - 1]
        else:
            out[i] = frame[j] * (1.0 - frac) + frame[j + 1] * frac
    return out
