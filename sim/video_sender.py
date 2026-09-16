"""模拟机器人图传发送端。

读取本地 H.265 裸流（Annex-B），按 access unit 切帧，
用 sim.video_protocol 的 8 字节大端头分片，通过 UDP 发往终端。
可注入丢包与抖动，用来验证终端在无重传链路下的表现。

用法：
    python -m sim.video_sender                 # 正常发送
    python -m sim.video_sender --loss 0.02     # 注入 2% 丢包
"""

from __future__ import annotations

import argparse
import random
import socket
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from sim.constants import SERVER_HOST, VIDEO_UDP_PORT
from sim.video_protocol import pack_frame

_START_CODE = b"\x00\x00\x01"
# HEVC NAL 类型 32=VPS 33=SPS 34=PPS，属于参数集，需与后续首个切片同帧发送
_PARAM_SET_TYPES = {32, 33, 34}


def _iter_nal_units(stream: bytes):
    pos = stream.find(_START_CODE)
    while pos >= 0:
        start = pos + len(_START_CODE)
        nxt = stream.find(_START_CODE, start)
        end = len(stream) if nxt < 0 else (nxt - 1 if stream[nxt - 1] == 0 else nxt)
        if end > start:
            yield stream[start:end]
        pos = nxt


def split_access_units(stream: bytes) -> list[bytes]:
    """把 Annex-B 裸流切成一帧一个 access unit（含前置参数集）。"""
    units: list[bytes] = []
    pending: list[bytes] = []
    for nal in _iter_nal_units(stream):
        nal_type = (nal[0] >> 1) & 0x3F
        is_slice = nal_type < 32
        if is_slice and pending and any(((n[0] >> 1) & 0x3F) < 32 for n in pending):
            units.append(b"".join(pending))
            pending = []
        pending.append(b"\x00\x00\x00\x01" + nal)
        if is_slice and nal_type not in _PARAM_SET_TYPES:
            units.append(b"".join(pending))
            pending = []
    if pending:
        units.append(b"".join(pending))
    return units


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default=str(Path(__file__).resolve().parents[1] / "assets" / "sim_feed.h265"))
    ap.add_argument("--host", default=SERVER_HOST)
    ap.add_argument("--port", type=int, default=VIDEO_UDP_PORT)
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--loss", type=float, default=0.0, help="模拟丢包率 0~1")
    ap.add_argument("--jitter-ms", type=float, default=0.0, help="每包随机抖动上限")
    args = ap.parse_args()

    path = Path(args.file)
    if not path.exists():
        print(f"[video_sender] 找不到码流文件 {path}", file=sys.stderr)
        return 1

    frames = split_access_units(path.read_bytes())
    if not frames:
        print("[video_sender] 未能从码流中切出任何帧", file=sys.stderr)
        return 1

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 21)
    rng = random.Random()
    target = (args.host, args.port)
    interval = 1.0 / args.fps

    print(f"[video_sender] {len(frames)} 帧 -> udp://{args.host}:{args.port}  "
          f"fps={args.fps} loss={args.loss:.1%} 循环播放，Ctrl-C 退出")

    frame_id = 0
    sent_packets = dropped = 0
    next_due = time.monotonic()
    try:
        while True:
            for au in frames:
                for pkt in pack_frame(frame_id, au):
                    if args.loss > 0 and rng.random() < args.loss:
                        dropped += 1
                        continue
                    sock.sendto(pkt, target)
                    sent_packets += 1
                    if args.jitter_ms > 0:
                        time.sleep(rng.uniform(0, args.jitter_ms) / 1000.0)
                frame_id = (frame_id + 1) & 0xFFFF
                if frame_id % 60 == 0:
                    print(f"[video_sender] 已发 {frame_id} 帧 {sent_packets} 包，丢弃 {dropped}")
                next_due += interval
                delay = next_due - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                else:
                    next_due = time.monotonic()
    except KeyboardInterrupt:
        print(f"\n[video_sender] 结束：{sent_packets} 包已发送，{dropped} 包主动丢弃")
    finally:
        sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
