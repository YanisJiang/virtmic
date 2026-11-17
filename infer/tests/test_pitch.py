import array
import math

from virtmic_infer.pitch import PitchShifter, reset_default_shifter, shift_semitones


def _sine_frame(freq_hz: float, n: int = 480, sr: int = 48000) -> array.array:
    return array.array(
        "f",
        [math.sin(2.0 * math.pi * freq_hz * i / sr) for i in range(n)],
    )


def _zcr(frame: array.array) -> int:
    c = 0
    for i in range(1, len(frame)):
        if (frame[i - 1] >= 0.0) != (frame[i] >= 0.0):
            c += 1
    return c


def test_zero_semitones_preserves_length():
    reset_default_shifter()
    frame = array.array("f", [0.1] * 480)
    # Prime then passthrough
    shifter = PitchShifter(capacity=480 * 8)
    for _ in range(8):
        out = shifter.process(frame, 0)
    assert len(out) == 480
    assert abs(out[0] - 0.1) < 1e-5


def test_plus_twelve_raises_zcr_across_frames():
    shifter = PitchShifter(capacity=480 * 8)
    base = _sine_frame(120.0)
    # Prime with zeros/semitone0
    for _ in range(8):
        shifter.process(base, 0)
    z0 = 0
    z12 = 0
    frames = 20
    for _ in range(frames):
        z0 += _zcr(shifter.process(base, 0))
    shifter.reset()
    for _ in range(8):
        shifter.process(base, 0)
    for _ in range(frames):
        z12 += _zcr(shifter.process(base, 12))
    # +12 octave ≈ double zero-crossings over the same duration.
    assert z12 > z0 * 1.5


def test_shift_semitones_changes_content_after_prime():
    reset_default_shifter()
    frame = _sine_frame(150.0)
    for _ in range(10):
        shift_semitones(frame, 0)
    out0 = shift_semitones(frame, 0)
    reset_default_shifter()
    for _ in range(10):
        shift_semitones(frame, 0)
    out7 = shift_semitones(frame, 7)
    assert len(out7) == 480
    assert list(out7) != list(out0)
