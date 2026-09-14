#!/usr/bin/env python3
"""Decode-ceiling analysis for the task-4 loss scenarios.

Explains why decoded_frames is low under loss without that being a receiver
defect. Correct reassembly delivers only WHOLE access units, so a lost fragment
removes an entire reference frame. The asset has no IRAP/keyframe NAL, so the
decoder can never re-sync. Byte-level truncation looks better only because it
hands FFmpeg partial NALs it can error-conceal, which a correct reassembler by
definition never emits.
"""
import random
import subprocess
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
from sim.video_sender import split_access_units

FRAME = 320 * 180 * 3
ASSET = os.path.join(ROOT, "assets", "sim_feed.h265")


def decode(stream):
    p = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "quiet", "-f", "hevc", "-i", "pipe:0",
         "-vf", "scale=320:180", "-f", "rawvideo", "-pix_fmt", "bgr24", "pipe:1"],
        input=stream, capture_output=True)
    return len(p.stdout) // FRAME


def main():
    data = open(ASSET, "rb").read()
    units = split_access_units(data)

    nals = []
    pos = 0
    while True:
        i = data.find(b"\x00\x00\x00\x01", pos)
        if i < 0:
            break
        if i + 5 < len(data):
            nals.append((data[i + 4] >> 1) & 0x3F)
        pos = i + 4
    irap = [t for t in nals if 16 <= t <= 21]
    with_ps = 0
    for u in units:
        p2, ts = 0, []
        while True:
            i = u.find(b"\x00\x00\x00\x01", p2)
            if i < 0:
                break
            if i + 5 < len(u):
                ts.append((u[i + 4] >> 1) & 0x3F)
            p2 = i + 4
        if any(32 <= t <= 34 for t in ts):
            with_ps += 1

    print(f"asset_nals={len(nals)} access_units={len(units)} "
          f"irap_keyframes={len(irap)} access_units_with_parameter_sets={with_ps}")
    print(f"clean_full_decode_frames={decode(data)}")

    for loss in (0.02, 0.10):
        for seed in (11, 12, 13):
            random.seed(seed)
            kept = [u for u in units
                    if all(random.random() >= loss for _ in range((len(u) + 1391) // 1392))]
            print(f"reassembly_semantics loss={loss:.2f} seed={seed} "
                  f"complete_access_units={len(kept)} ceiling_decoded={decode(b''.join(kept))}")

    for loss in (0.02, 0.10):
        random.seed(11)
        chunks = [data[i:i + 1392] for i in range(0, len(data), 1392)]
        kept = b"".join(c for c in chunks if random.random() >= loss)
        print(f"byte_truncation_semantics loss={loss:.2f} seed=11 "
              f"decoded={decode(kept)} note=not_achievable_by_correct_reassembly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
