#!/usr/bin/env python3
"""分席位验收：步兵、重装、指挥手各跑一局，断言 2027 协议的席位专属链路两头都通。

    .venv/bin/python scripts/accept_roles.py              # 三个席位
    .venv/bin/python scripts/accept_roles.py commander    # 只跑一个:infantry / hero / commander

每个席位自己起 broker、模拟服务器（RM_SELF_ID=该席位）和一个离屏终端，约 40 秒，三个席位
共约 2 分钟。离屏终端没法点按钮，所以指挥手和重装的指令由脚本按终端的编码直接发到
broker（编码本身由 role_commands_test 逐字段比对，按钮行为由 role_panes_test 覆盖）；
这里验的是模拟器按规则回状态、终端收到并解码。判定依据是终端日志：
startup（席位来源）、seat_confirmed（服务器报的编号和席位一致）、match_event（Event 解码后
的中文）、dart_status、sentry_result、map_mark_received，以及本车指令的 self_supply（经济/
允许发弹量/阵亡/读条满）、rune_status、performance；模拟器那一侧看它自己的日志和 sim/State。
已有模拟器在跑时拒绝启动。
"""

from __future__ import annotations

import json
import os
import platform
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "generated"))

import paho.mqtt.client as mqtt

import rm_terminal_pb2 as pb
from sim.constants import MQTT_PORT, SERVER_HOST, T_SIM_STATE

results: list[tuple[bool, str]] = []


def record(ok: bool, text: str) -> None:
    results.append((ok, text))
    print(f"{'PASS' if ok else 'FAIL'}  {text}", flush=True)


def wait_until(check, timeout: float) -> bool:
    start = time.monotonic()
    while time.monotonic() - start < timeout:
        if check():
            return True
        time.sleep(0.1)
    return check()


class Seat:
    """一个席位的一局:broker + 模拟器 + 离屏终端,结束时全部关掉。"""

    def __init__(self, seat: int, binary: Path, python: str, prep_sec: int = 2) -> None:
        self.seat = seat
        self.python = python
        self.run_dir = Path(tempfile.mkdtemp(prefix=f"rm-accept-roles-{seat}."))
        self.log_path = self.run_dir / "terminal.log"
        conf = self.run_dir / "terminal.conf"
        conf.write_text(f"mqtt_port={MQTT_PORT}\nlog_destination={self.log_path}\n"
                        f"speech_dir={self.run_dir / 'no-voice'}\n", encoding="utf-8")
        self.env = dict(os.environ, RM_SELF_ID=str(seat), QT_QPA_PLATFORM="offscreen",
                        RM_PREP_SEC=str(prep_sec), RM_CHECK_SEC="1", RM_COUNTDOWN_SEC="1")
        self.procs: list[subprocess.Popen] = []
        self.start("broker", ["mosquitto", "-p", str(MQTT_PORT)])
        time.sleep(0.8)
        self.start("server", [python, "-u", "-m", "sim.match_server"])
        time.sleep(1)
        self.terminal = self.start("terminal_stdout",
                                   [str(binary), "--config", str(conf), "--no-record"])
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"accept-roles-{seat}")
        self.sim_state: dict = {}
        self.client.on_message = self._on_sim_state
        self.client.connect(SERVER_HOST, MQTT_PORT, keepalive=10)
        self.client.subscribe(T_SIM_STATE)
        self.client.loop_start()

    def _on_sim_state(self, _client, _userdata, message) -> None:
        try:
            self.sim_state = json.loads(message.payload)
        except ValueError:
            pass

    def start(self, name: str, args: list[str]) -> subprocess.Popen:
        out = open(self.run_dir / f"{name}.log", "w", encoding="utf-8")
        proc = subprocess.Popen(args, cwd=ROOT, env=self.env, stdout=out, stderr=subprocess.STDOUT)
        self.procs.append(proc)
        return proc

    def log(self) -> str:
        return self.log_path.read_text(encoding="utf-8", errors="replace") if self.log_path.exists() else ""

    def has(self, pattern: str) -> bool:
        return re.search(pattern, self.log()) is not None

    def last(self, event: str) -> str:
        """终端日志里这类事件的最后一行;没有就是空串。"""
        lines = [line for line in self.log().splitlines() if f"event={event} " in line]
        return lines[-1] if lines else ""

    def server_has(self, pattern: str) -> bool:
        path = self.run_dir / "server.log"
        text = path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""
        return re.search(pattern, text) is not None

    def control(self, *args: str) -> None:
        subprocess.run([self.python, "sim/match_control.py", *args], cwd=ROOT, env=self.env,
                       capture_output=True, timeout=10, check=False)

    def send(self, topic: str, message) -> None:
        """按终端的方式发一条指令(topic = 消息名)。"""
        self.client.publish(topic, message.SerializeToString(), qos=0).wait_for_publish(timeout=3)

    def close(self) -> None:
        self.client.loop_stop()
        self.client.disconnect()
        for proc in reversed(self.procs):
            proc.terminate()
        for proc in self.procs:
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()


def common_checks(run: Seat, role: str) -> None:
    record(wait_until(lambda: run.has(rf"event=startup .*seat={run.seat} role={role} source=env"), 10),
           f"席位 {run.seat}:启动日志写明席位、角色({role})和来源(env),没有弹选席位页")
    record(wait_until(lambda: run.has(rf"event=seat_confirmed seat={run.seat} server={run.seat}"), 10),
           f"席位 {run.seat}:服务器 RobotStaticStatus 报的编号和席位一致,自检页显示「席位正确」")
    record(wait_until(lambda: run.has(r"event=ui_mode_switch|event=popup_switch .*to=None"), 15),
           f"席位 {run.seat}:比赛开始,终端在收数据")


def supply_checks(run: Seat) -> None:
    """兑换、远程兑换(6 秒后到账)、远程回血。开局 800 金币,没交过火算脱战。"""
    run.send("CommonCommand", pb.CommonCommand(cmd_type=1, param=50))
    record(wait_until(lambda: run.has(r"event=self_supply economy=750 ammo=50 "), 5),
           "兑换 50 发 17mm:扣 50 金币,允许发弹量 +50,终端收到")
    run.send("CommonCommand", pb.CommonCommand(cmd_type=5, param=100))
    record(wait_until(lambda: run.has(r"event=self_supply economy=600 ammo=50 "), 5),
           "远程兑换 100 发:当场扣 150 金币,发弹量先不到账")
    record(wait_until(lambda: run.has(r"event=self_supply economy=600 ammo=150 "), 9),
           "远程兑换 6 秒后到账:允许发弹量 150")
    run.control("hp", str(run.seat), "60")
    time.sleep(0.5)
    run.send("CommonCommand", pb.CommonCommand(cmd_type=6, param=0))
    record(wait_until(lambda: run.server_has(r"远程兑换血量 失败: 未脱战"), 5),
           "刚掉血就远程回血:模拟器按规则拒绝(交战中)")
    time.sleep(6.5)
    run.send("CommonCommand", pb.CommonCommand(cmd_type=6, param=0))
    record(wait_until(lambda: run.has(r"event=self_supply economy=5\d\d ammo=150 "), 5),
           "脱战后远程回血:扣金币,终端收到")
    record(wait_until(lambda: run.server_has(r"远程兑换血量到账 \+\d+"), 9), "远程回血 6 秒后到账")


def revive_checks(run: Seat) -> None:
    """阵亡后花金币立即复活;再阵亡等读条满,确认复活。"""
    run.control("hp", str(run.seat), "0")
    record(wait_until(lambda: run.has(r"event=self_supply economy=\d+ ammo=\d+ dead=1 free=0"), 5),
           "阵亡:RobotRespawnStatus 进入读条,终端收到")
    run.send("CommonCommand", pb.CommonCommand(cmd_type=3, param=0))
    record(wait_until(lambda: run.server_has(r"确认复活 失败: 读条未完成"), 5), "读条没满就确认复活:被拒")
    run.send("CommonCommand", pb.CommonCommand(cmd_type=4, param=0))
    record(wait_until(lambda: run.server_has(r"终端指令 兑换立即复活 成功"), 5)
           and wait_until(lambda: " dead=0 " in run.last("self_supply"), 5),
           "花金币立即复活:复活,终端收到")
    run.control("hp", str(run.seat), "0")
    run.control("speed", "10")
    ready = wait_until(lambda: run.has(r"event=self_supply economy=\d+ ammo=\d+ dead=1 free=1"), 10)
    run.control("speed", "1")
    record(ready, "再阵亡,读条满了也不自己复活:终端看到「可以确认复活」")
    run.send("CommonCommand", pb.CommonCommand(cmd_type=3, param=0))
    record(wait_until(lambda: run.server_has(r"终端指令 确认复活 成功"), 5)
           and wait_until(lambda: " dead=0 " in run.last("self_supply"), 5),
           "确认复活:复活,终端收到")


def keyboard_mouse_checks(run: Seat) -> None:
    """按终端的格式发 1.5 秒 75Hz 键鼠帧(W+A、左键、鼠标每帧 +3),模拟器统计到。"""
    frame = pb.KeyboardMouseControl(mouse_x=3, mouse_y=0, mouse_z=0, left_button_down=True,
                                    right_button_down=False, keyboard_value=0b101, mid_button_down=False)
    end = time.monotonic() + 1.5
    while time.monotonic() < end:
        run.client.publish("KeyboardMouseControl", frame.SerializeToString(), qos=0)
        time.sleep(1 / 75)
    km = (run.sim_state or {}).get("km") or {}
    record(60 <= km.get("hz", 0) <= 80 and km.get("keys") == "W A" and km.get("buttons") == "左",
           f"键鼠控制 75Hz:模拟器收到约 {km.get('hz')} 帧/秒,按键 {km.get('keys')!r},鼠标键 {km.get('buttons')!r}")
    record(km.get("mouse", [0])[0] >= 150, f"鼠标位移累积:一秒 x {km.get('mouse')}")
    time.sleep(1.6)
    record(not (run.sim_state or {}).get("km"), "停发后模拟器显示「终端没在发」")


def infantry(binary: Path, python: str) -> None:
    print("\n== 步兵(红方 3 号)", flush=True)
    run = Seat(3, binary, python, prep_sec=8)
    try:
        record(wait_until(lambda: run.has(r"event=seat_confirmed seat=3 server=3"), 10),
               "席位 3:连上模拟器,还在准备阶段")
        run.send("RobotPerformanceSelectionCommand", pb.RobotPerformanceSelectionCommand(shooter=2, chassis=2))
        record(wait_until(lambda: run.has(r"event=performance shooter=2 chassis=2"), 5),
               "准备阶段选性能体系(功率优先 + 爆发优先):RobotPerformanceSelectionSync 回显,终端收到")
        common_checks(run, "infantry")
        time.sleep(1.0)
        run.send("RobotPerformanceSelectionCommand", pb.RobotPerformanceSelectionCommand(shooter=1, chassis=1))
        record(wait_until(lambda: run.server_has(r"选性能体系失败: 只能在准备阶段开始后、比赛开始前选"), 5)
               and "shooter=2 chassis=2" in run.last("performance"),
               "开赛后再选被拒:性能体系锁定在功率优先 + 爆发优先")
        supply_checks(run)
        run.send("RuneActivateCommand", pb.RuneActivateCommand(activate=1))
        record(wait_until(lambda: run.has(r"event=rune_status status=2 "), 5),
               "开能量机关:RuneStatusSync 变成「正在激活」,终端收到")
        revive_checks(run)
        keyboard_mouse_checks(run)
        run.control("mark", "1")
        record(wait_until(lambda: run.has(r"event=map_mark_received kind=1 target=2 enemy=101"), 5),
               "收到指挥手标记(攻击 · 敌方 1 号重装),所有席位地图上都画")
        run.control("blind", "1", "2")
        record(wait_until(lambda: run.has(r"event=match_event id=9 .*敌方飞镖命中我方基地固定目标，遮挡 10 秒"), 5),
               "敌方飞镖命中:Event 9 解码成中文并算出遮挡 10 秒")
        record(run.terminal.poll() is None, "终端全程在运行")
    finally:
        run.close()


def hero(binary: Path, python: str) -> None:
    print("\n== 重装(蓝方 1 号)", flush=True)
    run = Seat(101, binary, python)
    try:
        common_checks(run, "hero")
        time.sleep(1.5)
        run.send("AssemblyCommand", pb.AssemblyCommand(operation=0, difficulty=1))
        time.sleep(0.5)
        run.send("AssemblyCommand", pb.AssemblyCommand(operation=2, difficulty=1))
        record(wait_until(lambda: run.has(r"event=match_event id=15 .*装配已主动退出"), 5),
               "开始兑换后取消:Event 15 原因 6「装配已主动退出」")
        run.send("AssemblyCommand", pb.AssemblyCommand(operation=0, difficulty=1))
        time.sleep(9.6)   # 模拟的科技核心每 3 秒走一步,三步走完才能确认
        run.send("AssemblyCommand", pb.AssemblyCommand(operation=1, difficulty=1))
        record(wait_until(lambda: run.has(r"event=match_event id=15 .*装配成功"), 5),
               "放入、平移、旋转走完后确认:Event 15「装配成功」")
        run.control("module")
        record(wait_until(lambda: run.has(r"event=match_event id=16 "), 5),
               "安装模块化 42mm 发射机构:Event 16")
        run.send("CommonCommand", pb.CommonCommand(cmd_type=2, param=5))
        record(wait_until(lambda: run.has(r"event=self_supply economy=\d+ ammo=5 "), 5)
               and run.server_has(r"终端指令 兑换 42mm x5 成功"),
               "重装兑换 5 发 42mm:允许发弹量 5,终端收到")
        run.send("CommonCommand", pb.CommonCommand(cmd_type=5, param=10))
        record(wait_until(lambda: run.has(r"event=self_supply economy=\d+ ammo=15 "), 9),
               "重装远程兑换 10 发 42mm:6 秒后到账 15")
        run.send("RobotPerformanceSelectionCommand", pb.RobotPerformanceSelectionCommand(shooter=2, chassis=2))
        run.send("RuneActivateCommand", pb.RuneActivateCommand(activate=1))
        record(wait_until(lambda: run.server_has(r"选性能体系失败: 只有步兵有性能体系"), 5)
               and wait_until(lambda: run.server_has(r"开能量机关失败: 只有步兵能开能量机关"), 5),
               "重装不能选性能体系、不能开能量机关:模拟器按规则拒绝")
        record(run.terminal.poll() is None, "终端全程在运行")
    finally:
        run.close()


def commander(binary: Path, python: str) -> None:
    print("\n== 指挥手(红方 6 号)", flush=True)
    run = Seat(6, binary, python)
    try:
        common_checks(run, "commander")
        run.send("DartCommand", pb.DartCommand(target_id=2, open=False, launch_confirm=False))
        record(wait_until(lambda: run.has(r"event=dart_status target=2 gate=0"), 5),
               "选飞镖目标:DartSelectTargetStatusSync 回显目标 2,闸门关闭")
        run.control("seek", "31")
        time.sleep(0.6)
        run.send("DartCommand", pb.DartCommand(target_id=2, open=True, launch_confirm=False))
        record(wait_until(lambda: run.has(r"event=dart_status target=2 gate=1"), 5), "开闸:闸门开启中")
        record(wait_until(lambda: run.has(r"event=dart_status target=2 gate=2"), 10), "7 秒后闸门已开启")
        run.send("DartCommand", pb.DartCommand(target_id=2, open=True, launch_confirm=True))
        record(wait_until(lambda: run.has(r"event=match_event id=9 .*我方飞镖命中敌方基地固定目标"), 5),
               "确认发射:Event 9 我方命中")
        time.sleep(1.0)
        record(not run.has(r"event=ui_mode_switch .*to=Video"), "我方命中不遮挡自己:没有切到图传全屏")
        run.send("SentryCtrlCommand", pb.SentryCtrlCommand(command_id=8))
        record(wait_until(lambda: run.has(r"event=sentry_result command=8 code=0"), 5),
               "哨兵切防御姿态:SentryCtrlResult 成功")
        time.sleep(1.2)
        run.send("SentryCtrlCommand", pb.SentryCtrlCommand(command_id=7))
        record(wait_until(lambda: run.has(r"event=sentry_result command=7 code=1"), 5),
               "5 秒内再切姿态:SentryCtrlResult 失败(冷却中)")
        run.send("MapClickCmd", pb.MapClickCmd(is_send_all=2, robot_id=bytes(7), mode=3, enemy_id=0,
                                               ascii=0, type=1, map_x=12.5, map_y=7.0))
        record(wait_until(lambda: run.has(r"event=map_mark_received kind=3 target=1 enemy=0 x=12.50 y=7.00"), 5),
               "地图标记:服务器回发 MapClickInfo,终端收到")
        record(run.terminal.poll() is None, "终端全程在运行")
    finally:
        run.close()


def main() -> int:
    build = "build/macos" if platform.system() == "Darwin" else "build/ubuntu"
    binary = Path(os.environ.get("RM_TERMINAL_BINARY", ROOT / build / "rm_terminal"))
    python = os.environ.get("RM_TERMINAL_PYTHON", str(ROOT / ".venv" / "bin" / "python"))
    if not binary.exists():
        print(f"FAIL 找不到终端 {binary}，先构建", file=sys.stderr)
        return 1
    with socket.socket() as sock:
        sock.settimeout(0.3)
        if sock.connect_ex((SERVER_HOST, MQTT_PORT)) == 0:
            print("FAIL 3333 端口已被占用：先关掉正在运行的模拟器（启动.command）", file=sys.stderr)
            return 1
    chosen = sys.argv[1:] or ["infantry", "hero", "commander"]
    runs = {"infantry": infantry, "hero": hero, "commander": commander}
    for name in chosen:
        if name not in runs:
            print(f"未知席位 {name}，可选 {' / '.join(runs)}", file=sys.stderr)
            return 2
        runs[name](binary, python)
        time.sleep(1)
    failed = [text for ok, text in results if not ok]
    print(f"\nRESULT: {'PASS' if not failed else 'FAIL'}（{len(results) - len(failed)}/{len(results)}）")
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())
