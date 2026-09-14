"""图传 UDP 分片打包与重组。

2026 通信协议规定每个 UDP 包前 8 个字节固定为（大端）：
    帧编号（递增）     : 2 byte
    当前帧内分片序号   : 2 byte
    当前帧总字节数     : 4 byte
每个 UDP 包最大 1400 字节，除去 8 字节固定头，数据部分 1392 字节。
图传无重传机制，因此接收端必须自行处理丢包、乱序、重复与超时。
"""

from __future__ import annotations

import struct
import time
from dataclasses import dataclass, field

from core.constants import (
    FRAME_ASSEMBLY_TIMEOUT_S,
    FRAME_CACHE_MAX,
    UDP_HEADER_SIZE,
    UDP_PAYLOAD_MAX,
)

# 大端：uint16 帧号, uint16 分片号, uint32 帧总字节数
_HEADER = struct.Struct(">HHI")
assert _HEADER.size == UDP_HEADER_SIZE


def pack_frame(frame_id: int, payload: bytes) -> list[bytes]:
    """把一帧编码数据切成带 8 字节大端头的 UDP 包列表。"""
    total = len(payload)
    packets: list[bytes] = []
    for index, offset in enumerate(range(0, total, UDP_PAYLOAD_MAX)):
        chunk = payload[offset : offset + UDP_PAYLOAD_MAX]
        packets.append(_HEADER.pack(frame_id & 0xFFFF, index, total) + chunk)
    return packets


@dataclass
class _Partial:
    """正在装配中的一帧。"""

    total_bytes: int
    first_seen: float
    chunks: dict[int, bytes] = field(default_factory=dict)

    @property
    def received_bytes(self) -> int:
        return sum(len(c) for c in self.chunks.values())

    def is_complete(self) -> bool:
        # 分片连续且字节数吻合才算收齐
        if self.received_bytes != self.total_bytes:
            return False
        expected = (self.total_bytes + UDP_PAYLOAD_MAX - 1) // UDP_PAYLOAD_MAX
        return len(self.chunks) == expected and max(self.chunks) == expected - 1

    def assemble(self) -> bytes:
        return b"".join(self.chunks[i] for i in sorted(self.chunks))


@dataclass
class ReassemblyStats:
    packets_received: int = 0
    packets_duplicated: int = 0
    packets_out_of_order: int = 0
    frames_completed: int = 0
    frames_dropped_incomplete: int = 0
    frames_dropped_stale: int = 0
    bytes_received: int = 0
    last_packet_time: float = 0.0

    @property
    def frame_loss_rate(self) -> float:
        attempted = self.frames_completed + self.frames_dropped_incomplete + self.frames_dropped_stale
        if attempted == 0:
            return 0.0
        return (self.frames_dropped_incomplete + self.frames_dropped_stale) / attempted


class FrameReassembler:
    """按帧号聚合分片，处理乱序、重复、缺片与超时。

    设计要点：
      - 收到属于更新帧的分片时，旧帧视为不可能再收齐，直接丢弃（图传无重传）。
      - 缓存条目有数量上限与超时，避免异常流量导致内存增长。
      - 只返回完整帧，残帧一律丢弃，不向解码器投喂半帧数据。
    """

    def __init__(self, timeout_s: float = FRAME_ASSEMBLY_TIMEOUT_S, cache_max: int = FRAME_CACHE_MAX) -> None:
        self._timeout_s = timeout_s
        self._cache_max = cache_max
        self._partials: dict[int, _Partial] = {}
        self._newest_frame_id: int | None = None
        self._last_completed_id: int | None = None
        self.stats = ReassemblyStats()

    def push(self, packet: bytes) -> bytes | None:
        """喂入一个 UDP 包，收齐一帧时返回该帧完整数据，否则返回 None。"""
        if len(packet) < UDP_HEADER_SIZE:
            return None

        now = time.monotonic()
        frame_id, frag_index, total_bytes = _HEADER.unpack_from(packet, 0)
        chunk = packet[UDP_HEADER_SIZE:]

        self.stats.packets_received += 1
        self.stats.bytes_received += len(packet)
        self.stats.last_packet_time = now

        # 该帧已收齐并交付，迟到/重发的分片属于重复数据
        if self._last_completed_id is not None and frame_id == self._last_completed_id:
            self.stats.packets_duplicated += 1
            return None

        if self._newest_frame_id is None or _newer(frame_id, self._newest_frame_id):
            self._newest_frame_id = frame_id
            self._discard_older_than(frame_id)
        elif frame_id != self._newest_frame_id:
            self.stats.packets_out_of_order += 1

        partial = self._partials.get(frame_id)
        if partial is None:
            partial = _Partial(total_bytes=total_bytes, first_seen=now)
            self._partials[frame_id] = partial
        elif frag_index in partial.chunks:
            self.stats.packets_duplicated += 1
            return None

        partial.chunks[frag_index] = chunk

        self._evict(now)

        if partial.is_complete():
            data = partial.assemble()
            del self._partials[frame_id]
            self._last_completed_id = frame_id
            self.stats.frames_completed += 1
            return data
        return None

    def _discard_older_than(self, frame_id: int) -> None:
        stale = [fid for fid in self._partials if _newer(frame_id, fid)]
        for fid in stale:
            del self._partials[fid]
            self.stats.frames_dropped_incomplete += 1

    def _evict(self, now: float) -> None:
        expired = [fid for fid, p in self._partials.items() if now - p.first_seen > self._timeout_s]
        for fid in expired:
            del self._partials[fid]
            self.stats.frames_dropped_stale += 1

        while len(self._partials) > self._cache_max:
            oldest = min(self._partials, key=lambda f: self._partials[f].first_seen)
            del self._partials[oldest]
            self.stats.frames_dropped_stale += 1


def _newer(a: int, b: int) -> bool:
    """16 位帧号的回绕安全比较：a 是否比 b 新。"""
    return ((a - b) & 0xFFFF) < 0x8000 and a != b
