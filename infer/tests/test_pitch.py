from virtmic_infer.pitch import shift_semitones
import array


def test_zero_semitones_preserves_length():
    frame = array.array("f", [0.1] * 480)
    out = shift_semitones(frame, 0)
    assert len(out) == 480
    assert abs(out[0] - 0.1) < 1e-6


def test_plus_twelve_changes_content():
    frame = array.array("f", [0.0] * 480)
    for i in range(480):
        frame[i] = 0.2 if (i // 40) % 2 == 0 else -0.2
    out = shift_semitones(frame, 12)
    assert len(out) == 480
    assert list(out) != list(frame)
