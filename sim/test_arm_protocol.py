"""sim/arm_protocol.py 的断言测试：`python -m sim.test_arm_protocol` 直接跑。

样帧与 cpp/referee_frame_test.cpp 用的是同一串字节：两边任何一边的 CRC 表或字段顺序
改错，这里或那边就会对不上。
"""

from __future__ import annotations

import math
import struct

from sim import arm_protocol

_FIRMWARE_FRAME = bytes.fromhex(
    "a51e0007fe0203cdcccc3dcdcc4c3e9a99993ecdcccc3e0000003f9a99193f0000000000004dee")


def test_crc_check_values() -> None:
    assert arm_protocol.crc16(b"123456789") == 0x6F91
    assert arm_protocol.crc8(b"123456789") == 0x0B


def test_frame_matches_the_terminal_test_vector() -> None:
    data = struct.pack("<6f", 0.1, 0.2, 0.3, 0.4, 0.5, 0.6) + bytes(6)
    frame = arm_protocol.encode_frame(arm_protocol.CMD_CUSTOM_CONTROLLER, data, 7)
    assert frame == _FIRMWARE_FRAME, frame.hex()


def test_joints_round_trip_in_degrees() -> None:
    payload = arm_protocol.joints_payload([0, 90, -45, 10, 0, 180])
    assert len(payload) == arm_protocol.DATA_BYTES
    back = arm_protocol.decode_joints_deg(payload)
    assert all(math.isclose(a, b, abs_tol=1e-4) for a, b in zip(back, [0, 90, -45, 10, 0, 180]))


def main() -> None:
    test_crc_check_values()
    print("crc check values ok")
    test_frame_matches_the_terminal_test_vector()
    print("frame matches terminal vector ok")
    test_joints_round_trip_in_degrees()
    print("joints round trip ok")


if __name__ == "__main__":
    main()
