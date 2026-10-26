"""假机械臂：在一个 pty 上按控制器固件的格式每秒发 25 帧 0x0302。

终端的 controller_serial 指向这里的符号链接，就和接了一根真的 USB 串口线一样。

    .venv/bin/python -m sim.fake_arm                        # 链接到 /tmp/rm_terminal_arm
    .venv/bin/python -m sim.fake_arm --path /tmp/arm --no-mqtt

控制面板（或 sim/links.py）通过 sim/Arm/* 控制它：
    sim/Arm/plugged  1 / 0   插回 / 拔线。拔线 = 关掉 pty 并删掉符号链接，终端那边读到
                             设备错误、再也找不到路径，和真的拔 USB 一样
    sim/Arm/joints   JSON    6 个关节角（度）
    sim/Arm/bad      任意    发一帧数据段被翻转一位的坏帧
"""

from __future__ import annotations

import argparse
import fcntl
import json
import os
import signal
import sys
import threading
import time
import tty
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from sim.arm_protocol import CMD_CUSTOM_CONTROLLER, encode_frame, joints_payload
from sim.constants import (ARM_SERIAL_PATH, MQTT_PORT, SERVER_HOST, T_SIM_ARM_BAD,
                           T_SIM_ARM_JOINTS, T_SIM_ARM_PLUGGED)


class FakeArm:
    def __init__(self, path: str) -> None:
        self.path = path
        self.lock = threading.Lock()
        self.joints_deg = [0.0] * 6
        self.bad_pending = 0
        self.master: int | None = None
        self.slave: int | None = None
        self.seq = 0
        self.sent = 0

    def plug(self) -> None:
        with self.lock:
            if self.master is not None:
                return
            master, slave = os.openpty()
            tty.setraw(slave)
            # 终端没在读的时候 pty 缓冲会写满；阻塞写会把这个进程卡死，所以写满就丢帧。
            flags = fcntl.fcntl(master, fcntl.F_GETFL)
            fcntl.fcntl(master, fcntl.F_SETFL, flags | os.O_NONBLOCK)
            if os.path.islink(self.path) or os.path.exists(self.path):
                os.remove(self.path)
            os.symlink(os.ttyname(slave), self.path)
            self.master, self.slave = master, slave
        print(f"[fake_arm] 插上 {self.path} -> {os.ttyname(slave)}", flush=True)

    def unplug(self) -> None:
        with self.lock:
            if self.master is None:
                return
            os.close(self.master)
            os.close(self.slave)
            self.master = self.slave = None
            if os.path.islink(self.path):
                os.remove(self.path)
        print("[fake_arm] 拔线", flush=True)

    def tick(self) -> None:
        with self.lock:
            if self.master is None:
                return
            frame = bytearray(encode_frame(CMD_CUSTOM_CONTROLLER, joints_payload(self.joints_deg),
                                           self.seq))
            self.seq = (self.seq + 1) & 0xFF
            if self.bad_pending:
                self.bad_pending -= 1
                frame[20] ^= 0x40
            try:
                os.write(self.master, bytes(frame))
                self.sent += 1
            except (BlockingIOError, OSError):
                pass


def control(arm: FakeArm) -> None:
    import paho.mqtt.client as mqtt

    def on_connect(client, _userdata, _flags, _reason, _properties=None):
        client.subscribe([(T_SIM_ARM_PLUGGED, 0), (T_SIM_ARM_JOINTS, 0), (T_SIM_ARM_BAD, 0)])

    def on_message(_client, _userdata, message):
        if message.topic == T_SIM_ARM_PLUGGED:
            (arm.plug if message.payload != b"0" else arm.unplug)()
        elif message.topic == T_SIM_ARM_JOINTS:
            try:
                values = [float(value) for value in json.loads(message.payload)][:6]
            except (ValueError, TypeError):
                return
            with arm.lock:
                arm.joints_deg = values + [0.0] * (6 - len(values))
        elif message.topic == T_SIM_ARM_BAD:
            with arm.lock:
                arm.bad_pending += 1
            print("[fake_arm] 发一帧坏帧", flush=True)

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="sim-fake-arm")
    client.on_connect = on_connect
    client.on_message = on_message
    while True:
        try:
            client.connect(SERVER_HOST, MQTT_PORT, keepalive=5)
            break
        except OSError:
            time.sleep(0.5)
    client.loop_forever(retry_first_connection=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--path", default=ARM_SERIAL_PATH)
    parser.add_argument("--hz", type=float, default=25.0, help="固件是 25Hz")
    parser.add_argument("--no-mqtt", action="store_true", help="不接控制面板，只按固定姿态发")
    args = parser.parse_args()

    # run.sh 用 SIGTERM 收尾。Python 默认直接退出、不走 finally,符号链接就会留下来指向
    # 一个之后可能分给别的终端会话的 /dev/pts/N —— 下次终端打开它会读走别人的键盘输入。
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))

    arm = FakeArm(args.path)
    arm.plug()
    if not args.no_mqtt:
        threading.Thread(target=control, args=(arm,), daemon=True).start()

    interval = 1.0 / args.hz
    due = time.monotonic()
    last_report = due
    try:
        while True:
            arm.tick()
            due += interval
            now = time.monotonic()
            if now - last_report >= 5:
                print(f"[fake_arm] 已发 {arm.sent} 帧", flush=True)
                last_report = now
            time.sleep(max(0.0, due - now))
    except KeyboardInterrupt:
        pass
    finally:
        arm.unplug()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
