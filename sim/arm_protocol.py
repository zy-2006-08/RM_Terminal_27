"""自定义控制器（小机械臂）的串口帧。

与控制器固件 RM_Lib.hpp 的 Data_Concatenation、终端 cpp/referee_frame.cpp 逐字节一致：

    0xA5 | u16 data_length | u8 seq | u8 CRC8 | u16 cmd_id | data | u16 CRC16

整数小端。CRC8 是 Maxim 反射多项式 0x8C、初值 0xFF；CRC16 是反射多项式 0x8408、
初值 0xFFFF。数据段 30 字节：6 个 float 关节角（弧度，0-3 机械臂关节、4 夹爪、5 臂 yaw），
后 6 字节固件没用到。

反方向是车的机械臂回传：6 个 float 关节角 + 1 字节标志位（2 = 让控制器跟随车），共 25 字节。
2027 协议取消了 0x0309，车把这 25 字节放进 0x0310（MQTT 上的 CustomByteBlock）发给终端，
终端再补零到 30 字节、按 0x0309 帧写给控制器。
"""

from __future__ import annotations

import math
import struct

SOF = 0xA5
CMD_CUSTOM_CONTROLLER = 0x0302
CMD_ARM_FEEDBACK = 0x0309
DATA_BYTES = 30
FEEDBACK_BYTES = 25
FOLLOW_FLAG = 2


def _table(poly: int) -> list[int]:
    table = []
    for index in range(256):
        crc = index
        for _ in range(8):
            crc = (crc >> 1) ^ poly if crc & 1 else crc >> 1
        table.append(crc)
    return table


_CRC8 = _table(0x8C)
_CRC16 = _table(0x8408)


def crc8(data: bytes) -> int:
    crc = 0xFF
    for byte in data:
        crc = _CRC8[crc ^ byte]
    return crc


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc = (crc >> 8) ^ _CRC16[(crc ^ byte) & 0xFF]
    return crc


def encode_frame(cmd_id: int, data: bytes, seq: int) -> bytes:
    header = bytes([SOF]) + struct.pack("<H", len(data)) + bytes([seq & 0xFF])
    header += bytes([crc8(header)])
    body = header + struct.pack("<H", cmd_id) + data
    return body + struct.pack("<H", crc16(body))


def joints_payload(joints_deg: list[float]) -> bytes:
    radians = [math.radians(value) for value in joints_deg]
    return struct.pack("<6f", *radians) + bytes(DATA_BYTES - 24)


def decode_joints_deg(data: bytes) -> list[float]:
    return [math.degrees(value) for value in struct.unpack_from("<6f", data)]


def feedback_payload(joints_deg: list[float], flag: int) -> bytes:
    radians = [math.radians(value) for value in joints_deg]
    return struct.pack("<6fB", *radians, flag & 0xFF)


def decode_feedback(data: bytes) -> tuple[list[float], int]:
    values = struct.unpack_from("<6fB", data)
    return [math.degrees(value) for value in values[:6]], values[6]


class FrameParser:
    """串口字节流 -> (cmd_id, data)。CRC 不对就丢掉这个 0xA5 往后找，和终端的解析器一样。"""

    def __init__(self) -> None:
        self.buffer = bytearray()
        self.bad_frames = 0

    def feed(self, data: bytes) -> list[tuple[int, bytes]]:
        self.buffer += data
        frames = []
        while True:
            start = self.buffer.find(SOF)
            if start < 0:
                self.buffer.clear()
                return frames
            del self.buffer[:start]
            if len(self.buffer) < 5:
                return frames
            if crc8(bytes(self.buffer[:4])) != self.buffer[4]:
                self.bad_frames += 1
                del self.buffer[:1]
                continue
            length = struct.unpack_from("<H", self.buffer, 1)[0]
            total = 5 + 2 + length + 2
            if length > 256:
                self.bad_frames += 1
                del self.buffer[:1]
                continue
            if len(self.buffer) < total:
                return frames
            frame = bytes(self.buffer[:total])
            if crc16(frame[:-2]) != struct.unpack_from("<H", frame, total - 2)[0]:
                self.bad_frames += 1
                del self.buffer[:1]
                continue
            frames.append((struct.unpack_from("<H", frame, 5)[0], frame[7:7 + length]))
            del self.buffer[:total]
