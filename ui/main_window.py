"""自定义终端主界面。

布局：
    左侧   图传画面 + 目标框/十字线/状态叠加
    右上   比赛信息（阶段、倒计时、比分）
    右中   机器人状态（HP、热量、弹量、功率）
    右下   自瞄面板 + 模块在线状态
    底部   模式切换按钮 + 报警条 + 链路统计
"""

from __future__ import annotations

import time

import cv2
import numpy as np
from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor, QFont, QImage, QKeySequence, QPainter, QPixmap, QShortcut
from PySide6.QtWidgets import (
    QApplication,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QProgressBar,
    QPushButton,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

from core.constants import (
    CHASSIS_MODES,
    CMD_EMERGENCY_STOP,
    CMD_RESET_GIMBAL,
    CMD_SET_CHASSIS_MODE,
    CMD_TOGGLE_AUTOAIM,
    GAME_STAGES,
    MODULE_LABELS,
    MODULE_STATE,
    SELF_ROBOT_NAME,
    VISION_TIMEOUT_S,
)
from core.mqtt_link import MqttLink
from core.video_receiver import VideoReceiver

C_BG = "#0A0E1A"
C_PANEL = "#1A1F2E"
C_BORDER = "#3A4151"
C_TEXT = "#E5E5EA"
C_DIM = "#8E8E93"
C_OK = "#30D158"
C_WARN = "#FFD60A"
C_BAD = "#FF453A"
C_ACCENT = "#00CED1"
C_BLUE = "#0A84FF"
C_INK = "#080B12"

STYLE = f"""
QWidget {{ background:{C_BG}; color:{C_TEXT};
           font-family:'PingFang SC','Helvetica Neue',sans-serif; }}
QFrame#panel {{ background:{C_PANEL}; border:1px solid {C_BORDER}; border-radius:12px; }}
QLabel#h {{ color:#8E959C; font-size:10px; font-weight:700; letter-spacing:1px; }}
QLabel#big {{ font-size:32px; font-weight:700; color:{C_TEXT}; }}
QLabel#mid {{ font-size:18px; font-weight:600; }}
QLabel#k {{ color:#8E959C; font-size:10px; }}
QLabel#v {{ font-family:'Consolas','SF Mono',monospace; font-size:14px; font-weight:600; }}
QPushButton {{ background:#202637; border:1px solid {C_BORDER}; border-radius:8px;
               padding:8px 12px; font-size:12px; font-weight:600; color:{C_TEXT}; }}
QPushButton:hover {{ background:#293247; border-color:{C_ACCENT}; }}
QPushButton:pressed {{ background:#111827; }}
QPushButton:checked {{ background:{C_ACCENT}; color:{C_INK}; border-color:{C_ACCENT}; }}
QPushButton#stop {{ background:#301B25; border-color:{C_BAD}; color:{C_BAD}; }}
QPushButton#stop:hover {{ background:{C_BAD}; color:#fff; }}
QProgressBar {{ background:#0D1220; border:1px solid {C_BORDER}; border-radius:5px;
                height:10px; text-align:center; font-size:10px; color:{C_DIM}; }}
QProgressBar::chunk {{ border-radius:4px; }}
"""


def _panel(title: str) -> tuple[QFrame, QVBoxLayout]:
    f = QFrame()
    f.setObjectName("panel")
    lay = QVBoxLayout(f)
    lay.setContentsMargins(12, 10, 12, 12)
    lay.setSpacing(7)
    head = QLabel(title)
    head.setObjectName("h")
    lay.addWidget(head)
    return f, lay


def _bar(color: str) -> QProgressBar:
    b = QProgressBar()
    b.setStyleSheet(f"QProgressBar::chunk{{background:{color};}}")
    return b


class VideoPane(QLabel):
    """图传显示区：解码画面 + 视觉叠加层。"""

    def __init__(self) -> None:
        super().__init__()
        self.setMinimumSize(760, 430)
        self.setAlignment(Qt.AlignCenter)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self.setStyleSheet(f"background:#05080c;border:1px solid {C_BORDER};border-radius:8px;")
        self.setText("等待图传码流...")
        self._src_size = (1280, 720)
        self._image = None
        self._overlay = None

    def show_frame(self, frame: np.ndarray, overlay) -> None:
        h, w = frame.shape[:2]
        self._src_size = (w, h)
        rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        self._image = QImage(rgb.data, w, h, 3 * w, QImage.Format_RGB888).copy()
        self._overlay = overlay
        self.update()

    def paintEvent(self, event) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)
        if self._image is None:
            painter.setPen(QColor(C_DIM)); painter.drawText(self.rect(), Qt.AlignCenter, self.text())
            return
        target = self._image.size()
        target.scale(self.size(), Qt.KeepAspectRatio)
        rect = __import__('PySide6.QtCore', fromlist=['QRect']).QRect(0, 0, target.width(), target.height())
        rect.moveCenter(self.rect().center())
        painter.drawImage(rect, self._image)
        if self._overlay:
            self._overlay(painter, rect, self._src_size[0], self._src_size[1])


class TerminalWindow(QMainWindow):
    def __init__(self, link: MqttLink, video: VideoReceiver) -> None:
        super().__init__()
        self.link = link
        self.video = video
        self._mode_buttons: dict[int, QPushButton] = {}
        self._last_lock_time = 0.0
        self._t0 = time.monotonic()

        self.setWindowTitle(f"RM2027 自定义终端 · {SELF_ROBOT_NAME}")
        self.resize(1500, 820)
        self.setStyleSheet(STYLE)
        self._build()

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._refresh)
        self._timer.start(33)

    # ------------------------------------------------------------ 布局
    def _build(self) -> None:
        root = QWidget()
        self.setCentralWidget(root)
        outer = QVBoxLayout(root)
        outer.setContentsMargins(12, 12, 12, 12)
        outer.setSpacing(10)

        outer.addWidget(self._build_status())

        body = QHBoxLayout()
        body.setSpacing(10)
        outer.addLayout(body, 1)

        left = QVBoxLayout()
        left.setSpacing(10)
        self.video_pane = VideoPane()
        left.addWidget(self.video_pane, 1)
        left.addWidget(self._build_controls())
        body.addLayout(left, 3)

        right = QVBoxLayout()
        right.setSpacing(10)
        right.addWidget(self._build_match())
        right.addWidget(self._build_robot())
        right.addWidget(self._build_autoaim())
        right.addWidget(self._build_modules())
        body.addLayout(right, 1)

    def _build_match(self):
        f, lay = _panel("比赛信息")
        self.l_stage = QLabel("--")
        self.l_stage.setObjectName("mid")
        self.l_clock = QLabel("--:--")
        self.l_clock.setObjectName("big")
        self.l_clock.setStyleSheet(f"color:{C_ACCENT};")
        self.l_score = QLabel("红 0 : 0 蓝")
        self.l_score.setObjectName("mid")
        for w in (self.l_stage, self.l_clock, self.l_score):
            lay.addWidget(w)
        return f

    def _build_robot(self):
        f, lay = _panel("机器人状态")
        grid = QGridLayout()
        grid.setSpacing(6)
        self.hp_bar = _bar(C_OK)
        self.heat_bar = _bar(C_WARN)
        self.l_hp = QLabel("-/-")
        self.l_heat = QLabel("-/-")
        self.l_ammo = QLabel("-")
        self.l_speed = QLabel("-")
        self.l_power = QLabel("-")
        self.l_coin = QLabel("-")
        rows = [
            ("血量", self.l_hp, self.hp_bar),
            ("热量", self.l_heat, self.heat_bar),
        ]
        r = 0
        for name, val, bar in rows:
            k = QLabel(name)
            k.setObjectName("k")
            val.setObjectName("v")
            grid.addWidget(k, r, 0)
            grid.addWidget(val, r, 1, alignment=Qt.AlignRight)
            grid.addWidget(bar, r + 1, 0, 1, 2)
            r += 2
        for name, val in [("允许发弹量", self.l_ammo), ("弹速", self.l_speed),
                          ("底盘功率", self.l_power), ("金币", self.l_coin)]:
            k = QLabel(name)
            k.setObjectName("k")
            val.setObjectName("v")
            grid.addWidget(k, r, 0)
            grid.addWidget(val, r, 1, alignment=Qt.AlignRight)
            r += 1
        lay.addLayout(grid)
        return f

    def _build_autoaim(self):
        f, lay = _panel("自瞄 / 视觉")
        self.l_aim_state = QLabel("未启用")
        self.l_aim_state.setObjectName("mid")
        lay.addWidget(self.l_aim_state)
        grid = QGridLayout()
        grid.setSpacing(6)
        self.l_target = QLabel("-")
        self.l_dist = QLabel("-")
        self.l_err = QLabel("-")
        self.l_conf = QLabel("-")
        self.l_gimbal = QLabel("-")
        self.l_friction = QLabel("-")
        for i, (name, val) in enumerate([
            ("目标 ID", self.l_target), ("距离", self.l_dist),
            ("角度误差", self.l_err), ("置信度", self.l_conf),
            ("云台 yaw/pitch", self.l_gimbal), ("摩擦轮", self.l_friction),
        ]):
            k = QLabel(name)
            k.setObjectName("k")
            val.setObjectName("v")
            grid.addWidget(k, i, 0)
            grid.addWidget(val, i, 1, alignment=Qt.AlignRight)
        lay.addLayout(grid)
        return f

    def _build_modules(self):
        f, lay = _panel("模块在线状态")
        grid = QGridLayout()
        grid.setSpacing(4)
        self._module_labels: dict[str, QLabel] = {}
        for i, (field, label) in enumerate(MODULE_LABELS):
            k = QLabel(label)
            k.setObjectName("k")
            v = QLabel("--")
            v.setObjectName("v")
            self._module_labels[field] = v
            grid.addWidget(k, i // 2, (i % 2) * 2)
            grid.addWidget(v, i // 2, (i % 2) * 2 + 1, alignment=Qt.AlignRight)
        lay.addLayout(grid)
        return f

    def _build_controls(self):
        f, lay = _panel("控制 (快捷键 F1-F5 切模式 · A 自瞄 · R 回中 · 空格 急停)")
        row = QHBoxLayout()
        row.setSpacing(7)
        for mode_id, name in CHASSIS_MODES.items():
            if mode_id == 0:
                continue
            b = QPushButton(f"F{mode_id} {name}")
            b.setCheckable(True)
            b.clicked.connect(lambda _=False, m=mode_id: self._set_mode(m))
            self._mode_buttons[mode_id] = b
            row.addWidget(b)
        lay.addLayout(row)

        row2 = QHBoxLayout()
        row2.setSpacing(7)
        self.b_aim = QPushButton("A 自瞄")
        self.b_aim.setCheckable(True)
        self.b_aim.clicked.connect(self._toggle_aim)
        b_reset = QPushButton("R 云台回中")
        b_reset.clicked.connect(lambda: self.link.send_custom_control(CMD_RESET_GIMBAL, 1))
        b_stop = QPushButton("空格 紧急停止")
        b_stop.setObjectName("stop")
        b_stop.clicked.connect(self._emergency)
        row2.addWidget(self.b_aim)
        row2.addWidget(b_reset)
        row2.addWidget(b_stop)
        lay.addLayout(row2)

        self.l_alert = QLabel("系统就绪")
        self.l_alert.setObjectName("v")
        lay.addWidget(self.l_alert)

        for key, fn in [
            ("F1", lambda: self._set_mode(1)), ("F2", lambda: self._set_mode(2)),
            ("F3", lambda: self._set_mode(3)), ("F4", lambda: self._set_mode(4)),
            ("A", self._toggle_aim_key), ("R", lambda: self.link.send_custom_control(CMD_RESET_GIMBAL, 1)),
            ("Space", self._emergency),
        ]:
            QShortcut(QKeySequence(key), self, activated=fn)
        return f

    def _build_status(self):
        f = QFrame()
        f.setObjectName("panel")
        lay = QHBoxLayout(f)
        lay.setContentsMargins(14, 9, 14, 9)
        lay.setSpacing(22)
        self.s_mqtt = QLabel("MQTT --")
        self.s_video = QLabel("图传 --")
        self.s_tele = QLabel("遥测 --")
        self.s_net = QLabel("链路 --")
        self.s_cmd = QLabel("已发指令 0")
        for w in (self.s_mqtt, self.s_video, self.s_tele, self.s_net, self.s_cmd):
            w.setObjectName("v")
            lay.addWidget(w)
        lay.addStretch(1)
        self.s_time = QLabel("")
        self.s_time.setObjectName("k")
        lay.addWidget(self.s_time)
        return f

    # ------------------------------------------------------------ 控制动作
    def _set_mode(self, mode: int) -> None:
        self.link.send_custom_control(CMD_SET_CHASSIS_MODE, mode)
        for m, b in self._mode_buttons.items():
            b.setChecked(m == mode)

    def _toggle_aim(self) -> None:
        self.link.send_custom_control(CMD_TOGGLE_AUTOAIM, 1 if self.b_aim.isChecked() else 0)

    def _toggle_aim_key(self) -> None:
        self.b_aim.setChecked(not self.b_aim.isChecked())
        self._toggle_aim()

    def _emergency(self) -> None:
        self.link.send_custom_control(CMD_EMERGENCY_STOP, 1)
        for b in self._mode_buttons.values():
            b.setChecked(False)
        self.b_aim.setChecked(False)

    # ------------------------------------------------------------ 刷新
    def _refresh(self) -> None:
        s = self.link.state.snapshot()
        with self.link.state:
            game = pb_copy(s.game)
            dyn = pb_copy(s.dynamic)
            mod = pb_copy(s.module)
            tel = pb_copy(s.telemetry)
            events = list(s.events[-6:])
            connected, tele_online = s.connected, s.telemetry_online
            tele_hz, msgs, cmds = s.telemetry_hz, s.messages_received, s.commands_sent

        frame = self.video.read()
        if frame is not None:
            self.video_pane.show_frame(frame, lambda c, w, h: self._draw_overlay(c, w, h, tel, dyn))

        self.l_stage.setText(GAME_STAGES.get(game.current_stage, "--"))
        self.l_clock.setText(f"{game.stage_countdown_sec // 60:02d}:{game.stage_countdown_sec % 60:02d}")
        self.l_score.setText(f"红 {game.red_score} : {game.blue_score} 蓝")

        hp_pct = int(100 * dyn.current_hp / dyn.max_hp) if dyn.max_hp else 0
        self.hp_bar.setValue(hp_pct)
        self.hp_bar.setStyleSheet(
            f"QProgressBar::chunk{{background:{C_OK if hp_pct > 50 else C_WARN if hp_pct > 20 else C_BAD};}}")
        self.l_hp.setText(f"{dyn.current_hp}/{dyn.max_hp}")

        heat_pct = int(100 * dyn.shooter_heat_17mm / dyn.shooter_heat_limit) if dyn.shooter_heat_limit else 0
        self.heat_bar.setValue(heat_pct)
        self.heat_bar.setStyleSheet(
            f"QProgressBar::chunk{{background:{C_OK if heat_pct < 60 else C_WARN if heat_pct < 85 else C_BAD};}}")
        self.l_heat.setText(f"{dyn.shooter_heat_17mm}/{dyn.shooter_heat_limit}")
        self.l_ammo.setText(str(dyn.remaining_ammo))
        self.l_speed.setText(f"{dyn.bullet_speed:.1f} m/s")
        self.l_power.setText(f"{dyn.chassis_power:.0f} W")
        self.l_coin.setText(str(dyn.coin))

        if not tel.vision_online:
            self.l_aim_state.setText("视觉离线")
            self.l_aim_state.setStyleSheet(f"color:{C_BAD};")
        elif tel.target_locked:
            self._last_lock_time = time.monotonic()
            self.l_aim_state.setText("已锁定目标")
            self.l_aim_state.setStyleSheet(f"color:{C_OK};")
        elif tel.autoaim_enabled:
            self.l_aim_state.setText("搜索目标")
            self.l_aim_state.setStyleSheet(f"color:{C_WARN};")
        else:
            self.l_aim_state.setText("自瞄未启用")
            self.l_aim_state.setStyleSheet(f"color:{C_DIM};")

        self.l_target.setText(str(tel.target_id) if tel.target_locked else "-")
        self.l_dist.setText(f"{tel.target_distance:.2f} m" if tel.target_locked else "-")
        self.l_err.setText(f"{tel.target_yaw_err:+.2f}° / {tel.target_pitch_err:+.2f}°"
                           if tel.target_locked else "-")
        self.l_conf.setText(f"{tel.confidence:.2f}" if tel.target_locked else "-")
        self.l_gimbal.setText(f"{tel.gimbal_yaw:+.1f}° / {tel.gimbal_pitch:+.1f}°")
        self.l_friction.setText(f"{tel.friction_rpm:.0f} rpm")

        for field, label in MODULE_LABELS:
            v = getattr(mod, field, 0)
            label_w = self._module_labels[field]
            label_w.setText(MODULE_STATE.get(v, "--"))
            label_w.setStyleSheet(f"color:{C_OK if v == 1 else C_WARN if v == 2 else C_BAD};")

        for m, b in self._mode_buttons.items():
            b.setChecked(m == tel.chassis_mode)
        self.b_aim.setChecked(bool(tel.autoaim_enabled))

        self._update_alert(connected, tele_online, tel, dyn, events)

        vs = self.video.stats
        self.s_mqtt.setText("MQTT 已连接" if connected else "MQTT 断开")
        self.s_mqtt.setStyleSheet(f"color:{C_OK if connected else C_BAD};")
        self.s_video.setText(f"图传 {vs.fps:.0f}fps {vs.bitrate_kbps / 1000:.1f}Mbps"
                             if vs.online else "图传 中断")
        self.s_video.setStyleSheet(f"color:{C_OK if vs.online else C_BAD};")
        self.s_tele.setText(f"遥测 {tele_hz:.0f}Hz" if tele_online else "遥测 超时")
        self.s_tele.setStyleSheet(f"color:{C_OK if tele_online else C_BAD};")
        self.s_net.setText(f"包 {vs.packets} · 丢帧 {vs.frame_loss_rate:.1%} · 重复 {vs.duplicated}")
        self.s_cmd.setText(f"已发指令 {cmds} · 收消息 {msgs}")
        self.s_time.setText(f"运行 {int(time.monotonic() - self._t0)}s")

    def _update_alert(self, connected, tele_online, tel, dyn, events) -> None:
        alerts: list[tuple[str, str]] = []
        if not connected:
            alerts.append((C_BAD, "MQTT 链路断开，已停止发送赛事指令"))
        if not self.video.stats.online:
            alerts.append((C_BAD, "图传中断"))
        if not tele_online:
            alerts.append((C_BAD, "机器人遥测超时，控制指令不再生效"))
        if not tel.vision_online:
            alerts.append((C_WARN, "视觉进程离线，自瞄已降级"))
        elif tel.autoaim_enabled and not tel.target_locked and \
                time.monotonic() - self._last_lock_time > VISION_TIMEOUT_S:
            alerts.append((C_WARN, "目标丢失"))
        if dyn.shooter_heat_limit and dyn.shooter_heat_17mm > 0.8 * dyn.shooter_heat_limit:
            alerts.append((C_WARN, "射击热量接近上限"))
        if dyn.max_hp and dyn.current_hp < 0.2 * dyn.max_hp:
            alerts.append((C_BAD, "血量偏低"))
        if dyn.remaining_ammo == 0:
            alerts.append((C_WARN, "允许发弹量为 0"))

        if alerts:
            color, text = alerts[0]
            extra = f"（另有 {len(alerts) - 1} 项）" if len(alerts) > 1 else ""
            self.l_alert.setText(f"⚠ {text}{extra}")
            self.l_alert.setStyleSheet(f"color:{color};")
        elif events:
            self.l_alert.setText(f"· {events[-1][2]}")
            self.l_alert.setStyleSheet(f"color:{C_DIM};")
        else:
            self.l_alert.setText("系统就绪")
            self.l_alert.setStyleSheet(f"color:{C_OK};")

    def _draw_overlay(self, canvas, w: int, h: int, tel, dyn) -> None:
        """在解码画面上叠加自制 UI：十字线、目标框、状态角标。"""
        sx, sy = w / 1920.0, h / 1080.0
        cx, cy = w // 2, h // 2

        cross = (90, 200, 90) if tel.fire_permit else (150, 150, 150)
        cv2.line(canvas, (cx - 26, cy), (cx - 7, cy), cross, 1, cv2.LINE_AA)
        cv2.line(canvas, (cx + 7, cy), (cx + 26, cy), cross, 1, cv2.LINE_AA)
        cv2.line(canvas, (cx, cy - 26), (cx, cy - 7), cross, 1, cv2.LINE_AA)
        cv2.line(canvas, (cx, cy + 7), (cx, cy + 26), cross, 1, cv2.LINE_AA)
        cv2.circle(canvas, (cx, cy), 2, cross, -1, cv2.LINE_AA)

        if tel.target_locked and tel.bbox_w > 0:
            bx, by = int(tel.bbox_cx * sx), int(tel.bbox_cy * sy)
            bw, bh = int(tel.bbox_w * sx / 2), int(tel.bbox_h * sy / 2)
            p1, p2 = (bx - bw, by - bh), (bx + bw, by + bh)
            color = (80, 220, 80) if tel.confidence > 0.85 else (60, 190, 230)
            for (ox, oy, dx, dy) in [(p1[0], p1[1], 1, 1), (p2[0], p1[1], -1, 1),
                                     (p1[0], p2[1], 1, -1), (p2[0], p2[1], -1, -1)]:
                seg = max(12, bw // 3)
                cv2.line(canvas, (ox, oy), (ox + dx * seg, oy), color, 2, cv2.LINE_AA)
                cv2.line(canvas, (ox, oy), (ox, oy + dy * seg), color, 2, cv2.LINE_AA)
            cv2.line(canvas, (cx, cy), (bx, by), color, 1, cv2.LINE_AA)
            tag = f"ID {tel.target_id}  {tel.target_distance:.2f}m  {tel.confidence:.2f}"
            cv2.putText(canvas, tag, (p1[0], max(18, p1[1] - 8)),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.52, color, 1, cv2.LINE_AA)

        badge = "AUTO AIM" if tel.autoaim_enabled else "MANUAL"
        bcol = (80, 220, 80) if tel.target_locked else (60, 190, 230) if tel.autoaim_enabled else (150, 150, 150)
        cv2.rectangle(canvas, (12, 12), (188, 40), (12, 16, 22), -1)
        cv2.putText(canvas, badge, (22, 32), cv2.FONT_HERSHEY_SIMPLEX, 0.58, bcol, 1, cv2.LINE_AA)

        mode = CHASSIS_MODES.get(tel.chassis_mode, "?")
        info = [
            f"MODE {tel.chassis_mode}",
            f"YAW {tel.gimbal_yaw:+6.1f}",
            f"PIT {tel.gimbal_pitch:+6.1f}",
            f"HP  {dyn.current_hp}",
            f"AMMO {dyn.remaining_ammo}",
        ]
        cv2.rectangle(canvas, (12, h - 20 - 22 * len(info)), (176, h - 12), (12, 16, 22), -1)
        for i, line in enumerate(info):
            cv2.putText(canvas, line, (22, h - 26 - 22 * (len(info) - 1 - i)),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.46, (200, 210, 220), 1, cv2.LINE_AA)

        if not tel.vision_online:
            cv2.putText(canvas, "VISION OFFLINE", (cx - 150, 48),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.9, (70, 70, 240), 2, cv2.LINE_AA)

    def closeEvent(self, event) -> None:
        self.video.stop()
        self.link.stop()
        super().closeEvent(event)


def pb_copy(msg):
    """回调线程可能正在改写消息对象，复制一份供 UI 读取。"""
    clone = type(msg)()
    clone.CopyFrom(msg)
    return clone
