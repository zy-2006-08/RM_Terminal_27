"""终端侧图传接收：UDP 收包 -> 分片重组 -> H.265 解码 -> BGR 图像。

解码走 FFmpeg 子进程（stdin 喂 Annex-B，stdout 出 rawvideo），
避免依赖 OpenCV 构建时是否启用了特定后端。
"""

from __future__ import annotations

import subprocess
import threading
import time
from dataclasses import dataclass

import numpy as np

from core.constants import (
    VIDEO_TIMEOUT_S,
    VIDEO_UDP_PORT,
)
from core.video_protocol import FrameReassembler


@dataclass
class VideoStats:
    fps: float = 0.0
    decoded_frames: int = 0
    bitrate_kbps: float = 0.0
    last_frame_time: float = 0.0
    packets: int = 0
    duplicated: int = 0
    out_of_order: int = 0
    frame_loss_rate: float = 0.0

    @property
    def online(self) -> bool:
        return self.last_frame_time > 0 and (time.monotonic() - self.last_frame_time) < VIDEO_TIMEOUT_S


class VideoReceiver:
    def __init__(self, width: int, height: int, port: int = VIDEO_UDP_PORT) -> None:
        self._width = width
        self._height = height
        self._port = port
        self._frame_bytes = width * height * 3
        self._reassembler = FrameReassembler()
        self.stats = VideoStats()

        self._latest: np.ndarray | None = None
        self._lock = threading.Lock()
        self._running = threading.Event()
        self._proc: subprocess.Popen | None = None
        self._threads: list[threading.Thread] = []

    def start(self) -> None:
        self._running.set()
        self._proc = subprocess.Popen(
            [
                "ffmpeg", "-hide_banner", "-loglevel", "quiet",
                "-fflags", "nobuffer", "-flags", "low_delay",
                "-f", "hevc", "-i", "pipe:0",
                "-f", "rawvideo", "-pix_fmt", "bgr24",
                "-s", f"{self._width}x{self._height}",
                "pipe:1",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        self._threads = [
            threading.Thread(target=self._recv_loop, name="udp-recv", daemon=True),
            threading.Thread(target=self._decode_loop, name="decode", daemon=True),
        ]
        for t in self._threads:
            t.start()

    def stop(self) -> None:
        self._running.clear()
        if self._proc:
            for stream in (self._proc.stdin, self._proc.stdout):
                try:
                    if stream:
                        stream.close()
                except OSError:
                    pass
            self._proc.terminate()
            try:
                self._proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self._proc.kill()

    def read(self) -> np.ndarray | None:
        with self._lock:
            return None if self._latest is None else self._latest.copy()

    def _recv_loop(self) -> None:
        import socket

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # 图传码率高，接收缓冲区过小会在内核层就丢包
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 22)
        sock.bind(("0.0.0.0", self._port))
        sock.settimeout(0.5)

        bytes_window = 0
        window_start = time.monotonic()
        try:
            while self._running.is_set():
                try:
                    packet, _ = sock.recvfrom(2048)
                except TimeoutError:
                    continue
                except OSError:
                    break

                bytes_window += len(packet)
                frame = self._reassembler.push(packet)
                if frame and self._proc and self._proc.stdin:
                    try:
                        self._proc.stdin.write(frame)
                        self._proc.stdin.flush()
                    except (BrokenPipeError, ValueError):
                        break

                now = time.monotonic()
                if now - window_start >= 1.0:
                    s = self._reassembler.stats
                    self.stats.bitrate_kbps = bytes_window * 8 / 1000.0 / (now - window_start)
                    self.stats.packets = s.packets_received
                    self.stats.duplicated = s.packets_duplicated
                    self.stats.out_of_order = s.packets_out_of_order
                    self.stats.frame_loss_rate = s.frame_loss_rate
                    bytes_window = 0
                    window_start = now
        finally:
            sock.close()

    def _decode_loop(self) -> None:
        assert self._proc and self._proc.stdout
        stdout = self._proc.stdout
        count_window = 0
        window_start = time.monotonic()

        while self._running.is_set():
            try:
                raw = stdout.read(self._frame_bytes)
            except (ValueError, OSError):
                break
            if not raw or len(raw) < self._frame_bytes:
                break

            img = np.frombuffer(raw, dtype=np.uint8).reshape((self._height, self._width, 3))
            with self._lock:
                self._latest = img
            self.stats.decoded_frames += 1
            self.stats.last_frame_time = time.monotonic()

            count_window += 1
            elapsed = self.stats.last_frame_time - window_start
            if elapsed >= 1.0:
                self.stats.fps = count_window / elapsed
                count_window = 0
                window_start = self.stats.last_frame_time
