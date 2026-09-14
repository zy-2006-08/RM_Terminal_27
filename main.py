"""自定义终端入口。

    python main.py                 # 默认 1280x720，连本地模拟环境
    python main.py --width 1920 --height 1080
"""

from __future__ import annotations

import argparse
import signal
import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(_ROOT))
sys.path.insert(0, str(_ROOT / "generated"))

from PySide6.QtWidgets import QApplication

from core.constants import MQTT_PORT, SERVER_HOST, VIDEO_UDP_PORT
from core.mqtt_link import MqttLink
from core.video_receiver import VideoReceiver
from ui.main_window import TerminalWindow


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=SERVER_HOST)
    ap.add_argument("--mqtt-port", type=int, default=MQTT_PORT)
    ap.add_argument("--video-port", type=int, default=VIDEO_UDP_PORT)
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    args = ap.parse_args()

    signal.signal(signal.SIGINT, signal.SIG_DFL)

    app = QApplication(sys.argv)

    video = VideoReceiver(args.width, args.height, port=args.video_port)
    video.start()

    link = MqttLink(host=args.host, port=args.mqtt_port)
    link.start()

    win = TerminalWindow(link, video)
    screen = app.primaryScreen().availableGeometry()
    win.resize(min(1480, screen.width() - 60), min(780, screen.height() - 60))
    win.move(screen.x() + (screen.width() - win.width()) // 2, screen.y() + 20)
    win.show()
    print(f"[terminal] UI 已启动  图传 udp://0.0.0.0:{args.video_port}  "
          f"MQTT {args.host}:{args.mqtt_port}")
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
