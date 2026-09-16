"""模拟器共享常量与协议参数。

数值来源 RoboMaster 2026 通信协议 V2.0.0（20260626）：
  - 服务器 IP 192.168.12.1，MQTT 端口 3333，自定义客户端 IP 192.168.12.2
  - 图传码流：UDP 3334，1920x1080，60fps，HEVC(H.265)，无重传
  - 每个 UDP 包最大 1400 字节，其中前 8 字节为大端头，数据部分 1392 字节
RM2027 正式协议尚未发布，本地模拟统一走 127.0.0.1。

原先位于 core/constants.py。core/ 承载的是已移除的 Python/PySide6 终端；
原生 C++ 终端从 rm_terminal.conf 读取自己的配置，因此这里只保留模拟器
（sim/match_server.py、sim/video_sender.py、sim/video_protocol.py）实际
引用的常量。终端侧的显示词表由 cpp/presentation.cpp 承担，不在此重复。
"""

# ---------------- 网络 ----------------
# 真实比赛（2026）为 192.168.12.1 / .2，本地模拟用回环
SERVER_HOST = "127.0.0.1"
MQTT_PORT = 3333          # 2026 协议规定的 MQTT 端口
VIDEO_UDP_PORT = 3334     # 2026 协议规定的图传码流 UDP 端口

# ---------------- 图传分片 ----------------
UDP_PACKET_MAX = 1400     # 单个 UDP 包上限
UDP_HEADER_SIZE = 8       # 帧编号2 + 分片序号2 + 帧总字节数4，大端
UDP_PAYLOAD_MAX = UDP_PACKET_MAX - UDP_HEADER_SIZE   # 1392

# 重组器参数
FRAME_ASSEMBLY_TIMEOUT_S = 0.30   # 超过此时间未收齐则丢弃该帧
FRAME_CACHE_MAX = 8               # 同时在装配中的帧数上限，防止内存增长

# ---------------- 本机机器人身份 ----------------
# 附录二 ID 编号：1红英雄 2红工程 3/4/5红步兵 6红空中 7红哨兵
SELF_ROBOT_ID = 3         # 红方 3 号步兵

# ---------------- MQTT topic（与 2026 协议一致，topic 即指令名）----------------
T_GAME_STATUS = "GameStatus"
T_ROBOT_DYNAMIC = "RobotDynamicStatus"
T_ROBOT_MODULE = "RobotModuleStatus"
T_ROBOT_POSITION = "RobotPosition"
T_EVENT = "Event"
T_TELEMETRY = "RobotTelemetry"        # 机器人 -> 终端（对应 0x0310）
T_BLIND_STATUS = "BlindStatus"
T_ROBOT_POSITION_SET = "RobotPositionSet"
T_ROBOT_HEALTH_SET = "RobotHealthSet"

# 终端 -> 机器人 / 服务器。原生终端是只读的，永不发布这两个 topic；
# 模拟器订阅它们只为断言「终端确实没有发出任何控制指令」。
T_CUSTOM_CONTROL = "CustomControl"    # 对应 0x0311
T_COMMON_COMMAND = "CommonCommand"
