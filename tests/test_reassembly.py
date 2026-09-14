"""重组器行为测试：正常、乱序、重复、缺片、超时、帧号回绕。

运行： venv/bin/python -m tests.test_reassembly
"""

import random
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from core.constants import UDP_PACKET_MAX, UDP_PAYLOAD_MAX
from core.video_protocol import FrameReassembler, pack_frame


def _payload(n: int) -> bytes:
    return bytes((i * 7 + 13) & 0xFF for i in range(n))


def test_packet_size_within_limit() -> None:
    packets = pack_frame(1, _payload(50_000))
    assert all(len(p) <= UDP_PACKET_MAX for p in packets), "UDP 包超过 1400 字节上限"
    assert len(packets) == (50_000 + UDP_PAYLOAD_MAX - 1) // UDP_PAYLOAD_MAX
    print(f"  分片数 {len(packets)}，最大包 {max(len(p) for p in packets)} 字节")


def test_in_order_roundtrip() -> None:
    data = _payload(40_000)
    r = FrameReassembler()
    out = [r.push(p) for p in pack_frame(7, data)]
    completed = [o for o in out if o is not None]
    assert len(completed) == 1 and completed[0] == data, "顺序到达未能还原原始帧"
    print(f"  完整还原 {len(data)} 字节")


def test_out_of_order() -> None:
    data = _payload(30_000)
    packets = pack_frame(9, data)
    random.Random(42).shuffle(packets)
    r = FrameReassembler()
    got = None
    for p in packets:
        got = r.push(p) or got
    assert got == data, "乱序到达未能还原"
    print(f"  乱序还原成功，乱序计数 {r.stats.packets_out_of_order}")


def test_duplicates_ignored() -> None:
    data = _payload(5_000)
    packets = pack_frame(11, data)
    r = FrameReassembler()
    got = None
    for p in packets + packets:  # 每个包发两次
        got = r.push(p) or got
    assert got == data
    assert r.stats.packets_duplicated > 0, "未统计到重复包"
    print(f"  重复包被忽略 {r.stats.packets_duplicated} 个")


def test_missing_fragment_drops_frame() -> None:
    r = FrameReassembler()
    bad = pack_frame(20, _payload(20_000))
    del bad[2]                       # 人为丢一个中间分片
    for p in bad:
        assert r.push(p) is None, "缺片帧不应被判定为完整"

    good = _payload(9_000)
    out = None
    for p in pack_frame(21, good):   # 下一帧到达，旧残帧应被丢弃
        out = r.push(p) or out
    assert out == good
    assert r.stats.frames_dropped_incomplete >= 1, "未统计到缺片丢帧"
    print(f"  缺片帧被丢弃，丢帧率 {r.stats.frame_loss_rate:.1%}")


def test_stale_timeout() -> None:
    r = FrameReassembler(timeout_s=0.05)
    partial = pack_frame(30, _payload(20_000))
    r.push(partial[0])
    time.sleep(0.08)
    r.push(partial[1])               # 触发超时清理
    assert r.stats.frames_dropped_stale >= 1, "未触发超时丢帧"
    print(f"  超时丢帧 {r.stats.frames_dropped_stale} 帧")


def test_frame_id_wraparound() -> None:
    data = _payload(3_000)
    r = FrameReassembler()
    for fid in (65534, 65535, 0, 1):   # 跨越 16 位回绕
        out = None
        for p in pack_frame(fid, data):
            out = r.push(p) or out
        assert out == data, f"帧号 {fid} 处理失败"
    print("  帧号 65535 -> 0 回绕正常")


def test_random_packet_loss() -> None:
    """模拟 3% 丢包，验证不崩溃且统计合理。"""
    rng = random.Random(7)
    r = FrameReassembler()
    for fid in range(60):
        for p in pack_frame(fid, _payload(12_000)):
            if rng.random() < 0.03:
                continue
            r.push(p)
    s = r.stats
    assert s.frames_completed > 0, "全部帧丢失，重组器逻辑有问题"
    print(f"  3% 丢包下：完整 {s.frames_completed} 帧，丢弃 {s.frames_dropped_incomplete + s.frames_dropped_stale} 帧，丢帧率 {s.frame_loss_rate:.1%}")


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in tests:
        print(f"[RUN ] {fn.__name__}")
        fn()
        print(f"[PASS] {fn.__name__}\n")
    print(f"全部 {len(tests)} 项测试通过")
