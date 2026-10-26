"""自定义控制器（小机械臂）的串口帧。

与控制器固件 RM_Lib.hpp 的 Data_Concatenation、终端 cpp/referee_frame.cpp 逐字节一致：

    0xA5 | u16 data_length | u8 seq | u8 CRC8 | u16 cmd_id | data | u16 CRC16

整数小端。CRC8 是 Maxim 反射多项式 0x8C、初值 0xFF；CRC16 是反射多项式 0x8408、
初值 0xFFFF。数据段 30 字节：6 个 float 关节角（弧度，0-3 机械臂关节、4 夹爪、5 臂 yaw），
后 6 字节固件没用到。
"""

from __future__ import annotations

import math
import struct

SOF = 0xA5
CMD_CUSTOM_CONTROLLER = 0x0302
DATA_BYTES = 30


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
