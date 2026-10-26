"""终端和 Broker 之间的一跳 TCP 转发，用来模拟「裁判数据断了」。

    ok      正常转发
    cut     关掉正在转发的连接，新连接一律拒绝（网线被拔、服务器重启）
    freeze  连接不断，但两个方向都不再转发任何字节（链路黑洞）：终端只能靠
            MQTT 保活超时才发现断了，这正是最难发现的那种断线

模式来自 retained topic sim/Link/mqtt。只断终端这一跳：模拟器、控制面板都直连
Broker，所以断开以后按钮还能把它接回来。

    .venv/bin/python -m sim.link_proxy        # 监听 3335，转发到 3333
"""

from __future__ import annotations

import argparse
import socket
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from sim.constants import MQTT_PORT, MQTT_PROXY_PORT, SERVER_HOST, T_SIM_LINK_MQTT


class Pair:
    def __init__(self, client: socket.socket, upstream: socket.socket, frozen: bool) -> None:
        self.client = client
        self.upstream = upstream
        self.frozen = frozen
        self.closed = False

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        for sock in (self.client, self.upstream):
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()


class Proxy:
    def __init__(self, listen_port: int, upstream_port: int) -> None:
        self.listen_port = listen_port
        self.upstream_port = upstream_port
        self.mode = "ok"
        self.pairs: list[Pair] = []
        self.lock = threading.Lock()

    def set_mode(self, mode: str) -> None:
        if mode not in ("ok", "cut", "freeze"):
            return
        with self.lock:
            previous, self.mode = self.mode, mode
            pairs = list(self.pairs)
        if mode == previous:
            return
        print(f"[link_proxy] 裁判数据链路 {previous} -> {mode}", flush=True)
        for pair in pairs:
            if mode == "cut":
                pair.close()
            elif mode == "freeze":
                pair.frozen = True
            elif pair.frozen:
                # 黑洞期间丢掉了字节，这条 MQTT 字节流已经错位，只能断开让终端重连。
                pair.close()

    def pump(self, pair: Pair, source: socket.socket, target: socket.socket) -> None:
        try:
            while True:
                data = source.recv(65536)
                if not data:
                    break
                if not pair.frozen:
                    target.sendall(data)
        except OSError:
            pass
        finally:
            pair.close()
            with self.lock:
                if pair in self.pairs:
                    self.pairs.remove(pair)

    def serve(self) -> None:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((SERVER_HOST, self.listen_port))
        listener.listen(8)
        print(f"[link_proxy] {SERVER_HOST}:{self.listen_port} -> {SERVER_HOST}:{self.upstream_port}",
              flush=True)
        while True:
            client, _ = listener.accept()
            with self.lock:
                mode = self.mode
            if mode == "cut":
                client.close()
                continue
            try:
                upstream = socket.create_connection((SERVER_HOST, self.upstream_port), timeout=2)
                upstream.settimeout(None)
            except OSError:
                client.close()
                continue
            pair = Pair(client, upstream, frozen=mode == "freeze")
            with self.lock:
                self.pairs.append(pair)
            threading.Thread(target=self.pump, args=(pair, client, upstream), daemon=True).start()
            threading.Thread(target=self.pump, args=(pair, upstream, client), daemon=True).start()


def control(proxy: Proxy) -> None:
    import paho.mqtt.client as mqtt

    def on_connect(client, _userdata, _flags, _reason, _properties=None):
        client.subscribe(T_SIM_LINK_MQTT)

    def on_message(_client, _userdata, message):
        proxy.set_mode(message.payload.decode(errors="replace"))

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="sim-link-proxy")
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
    parser.add_argument("--listen", type=int, default=MQTT_PROXY_PORT)
    parser.add_argument("--upstream", type=int, default=MQTT_PORT)
    args = parser.parse_args()
    proxy = Proxy(args.listen, args.upstream)
    threading.Thread(target=control, args=(proxy,), daemon=True).start()
    try:
        proxy.serve()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
