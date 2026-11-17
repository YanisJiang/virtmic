"""Entry: python -m virtmic_infer [--semitones N]"""

from __future__ import annotations

import argparse
import array
import sys
import threading
import time

from virtmic_infer.control import PitchControlServer
from virtmic_infer.pitch import PitchShifter
from virtmic_infer.shm import (
    IN_MAGIC,
    IN_NAME,
    OUT_MAGIC,
    OUT_NAME,
    SAMPLES_PER_FRAME,
    ShmRing,
)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="virtmic fake-pitch infer worker")
    parser.add_argument("--semitones", type=int, default=7)
    args = parser.parse_args(argv)

    pitch_lock = threading.Lock()
    state = {"semitones": int(args.semitones)}
    shifter = PitchShifter()

    def on_pitch(v: int) -> None:
        clamped = max(-12, min(12, int(v)))
        with pitch_lock:
            state["semitones"] = clamped
        print(f"virtmic-infer: pitch={clamped}", flush=True)

    in_ring = None
    out_ring = None
    deadline = time.time() + 30.0
    while time.time() < deadline:
        try:
            in_ring = ShmRing(IN_NAME, IN_MAGIC)
            out_ring = ShmRing(OUT_NAME, OUT_MAGIC)
            break
        except OSError as exc:
            print(f"virtmic-infer: waiting for SHM ({exc})", flush=True)
            time.sleep(0.25)
    if in_ring is None or out_ring is None:
        print("virtmic-infer: failed to open infer SHM rings", file=sys.stderr)
        return 1

    ctl = PitchControlServer(on_pitch)
    ctl.start()
    with pitch_lock:
        cur = state["semitones"]
    print(
        f"virtmic-infer: listening on \\\\.\\pipe\\VirtMicInferControl pitch={cur}",
        flush=True,
    )

    try:
        while True:
            got = in_ring.try_read_latest()
            if not got:
                time.sleep(0.001)
                continue
            pcm_bytes, _seq = got
            frame = array.array("f")
            frame.frombytes(pcm_bytes)
            if len(frame) != SAMPLES_PER_FRAME:
                frame = array.array("f", list(frame)[:SAMPLES_PER_FRAME])
                while len(frame) < SAMPLES_PER_FRAME:
                    frame.append(0.0)
            with pitch_lock:
                st = state["semitones"]
            out = shifter.process(frame, st)
            out_ring.write_frame(out.tobytes())
    except KeyboardInterrupt:
        print("virtmic-infer: stopping", flush=True)
    finally:
        ctl.stop()
        in_ring.close()
        out_ring.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
