"""模拟器共享常量与协议参数。

数值来源 RoboMaster 2027 自定义终端通信协议 V1.0.0：
  - 服务器 IP 192.168.12.1，MQTT 端口 3333，自定义客户端 IP 192.168.12.2
  - 图传码流：UDP 3334，1920x1080，60fps，HEVC(H.265)，无重传
  - 每个 UDP 包最大 1400 字节，其中前 8 字节为大端头，数据部分 1392 字节
本地模拟统一走 127.0.0.1。

原先位于 core/constants.py。core/ 承载的是已移除的 Python/PySide6 终端；
原生 C++ 终端从 rm_terminal.conf 读取自己的配置，因此这里只保留模拟器
（sim/match_server.py、sim/video_sender.py、sim/video_protocol.py）实际
引用的常量。终端侧的显示词表由 cpp/presentation.cpp 承担，不在此重复。
"""

import os

# ---------------- 网络 ----------------
# 真实比赛为 192.168.12.1 / .2，本地模拟用回环
SERVER_HOST = "127.0.0.1"
MQTT_PORT = 3333
VIDEO_UDP_PORT = 3334

# ---------------- 图传分片 ----------------
UDP_PACKET_MAX = 1400     # 单个 UDP 包上限
UDP_HEADER_SIZE = 8       # 帧编号2 + 分片序号2 + 帧总字节数4，大端
UDP_PAYLOAD_MAX = UDP_PACKET_MAX - UDP_HEADER_SIZE   # 1392

# 重组器参数
FRAME_ASSEMBLY_TIMEOUT_S = 0.30   # 超过此时间未收齐则丢弃该帧
FRAME_CACHE_MAX = 8               # 同时在装配中的帧数上限，防止内存增长

# ---------------- 本机机器人身份 ----------------
# 附录二 ID 编号：1红英雄 2红工程 3/4/5红步兵 6红空中 7红哨兵；蓝方 +100。
# RM_SELF_ID 可把本机切到任一 2027 阵容编号，终端配置项与之一致。
SELF_ROBOT_ID = int(os.environ.get("RM_SELF_ID", "3"))

# ---------------- MQTT topic（2027 V1.0.0：topic 即消息名）----------------
T_GAME_STATUS = "GameStatus"
T_GLOBAL_UNIT = "GlobalUnitStatus"
T_GLOBAL_LOGISTICS = "GlobalLogisticsStatus"
T_GLOBAL_SPECIAL = "GlobalSpecialMechanism"
T_ROBOT_DYNAMIC = "RobotDynamicStatus"
T_ROBOT_RESPAWN = "RobotRespawnStatus"
T_ROBOT_STATIC = "RobotStaticStatus"
T_ROBOT_MODULE = "RobotModuleStatus"
T_ROBOT_POSITION = "RobotPosition"
T_RADAR_INFO = "RadarInfoToClient"
T_RUNE_STATUS = "RuneStatusSync"
T_PERFORMANCE_SYNC = "RobotPerformanceSelectionSync"  # 步兵性能体系，1Hz
T_TECH_CORE = "TechCoreMotionStateSync"
T_SENTRY_STATUS = "SentryStatusSync"
T_PATH_PLAN = "RobotPathPlanInfo"
T_DART_STATUS = "DartSelectTargetStatusSync"
T_MAP_INFO = "MapClickInfo"
T_SENTRY_RESULT = "SentryCtrlResult"
T_EVENT = "Event"
T_CUSTOM_BYTE_BLOCK = "CustomByteBlock"  # 机器人 -> 终端（0x0310）；重装放机械臂回传 25 字节
# 仅模拟器使用:服务器把随机掉血开关回显出来,控制面板的勾选状态才不会和服务器分叉。
T_SIM_AUTO_HP = "sim/AutoHP"
# 仅模拟器使用:服务器按绝对红蓝视角发的状态快照(JSON,2Hz),给控制面板和验收脚本读。
# 协议里的「己方/对方」是相对自机的,面板要的是红蓝,所以单独发一份,终端不订阅。
T_SIM_STATE = "sim/State"

# 终端 -> 机器人 / 服务器。模拟器订阅并执行它们；控制面板的赛程指令也走 CommonCommand。
T_CUSTOM_CONTROL = "CustomControl"    # 对应 0x0311；载荷是机械臂 0x0302 帧的 30 字节数据段
T_COMMON_COMMAND = "CommonCommand"
T_ASSEMBLY_COMMAND = "AssemblyCommand"
T_PERFORMANCE_COMMAND = "RobotPerformanceSelectionCommand"
T_RUNE_COMMAND = "RuneActivateCommand"
T_SENTRY_COMMAND = "SentryCtrlCommand"
T_DART_COMMAND = "DartCommand"
T_MAP_COMMAND = "MapClickCmd"
T_KEYBOARD_MOUSE = "KeyboardMouseControl"  # 2.2.1，75Hz；真服务器经图传转给车，模拟器只统计和显示

# ---------------- 链路故障注入（仅模拟器）----------------
# 控制面板和 sim/links.py 往这些 topic 写状态，各模拟进程据此断开自己负责的那条链路。
T_SIM_LINK_MQTT = "sim/Link/mqtt"     # retained: ok / cut / freeze（终端到 Broker 那一跳）
T_SIM_LINK_CAR = "sim/Link/car"       # retained: ok / off（车端 CustomByteBlock 停发）
T_SIM_LINK_VIDEO = "sim/Link/video"   # retained: ok / off（图传停发）
T_SIM_ARM_PLUGGED = "sim/Arm/plugged" # retained: 1 / 0（假机械臂插着 / 拔了）
T_SIM_ARM_JOINTS = "sim/Arm/joints"   # retained: JSON 6 个关节角（度）
T_SIM_ARM_BAD = "sim/Arm/bad"         # 每收到一条就发一帧校验错的数据
T_SIM_ARM_ECHO = "sim/ArmEcho"        # 模拟的车收到的机械臂姿态，JSON
T_SIM_ARM_FOLLOW = "sim/Arm/follow"   # retained: 1 / 0（让模拟的车回传「跟随」标志 2）
T_SIM_ARM_FEEDBACK = "sim/Arm/feedback"  # 假机械臂收到的车的回传（终端转来的 0x0309），JSON

# 模拟环境里终端连这个端口，经 sim/link_proxy.py 转发到 Broker。这样「断开裁判数据」
# 只断终端这一跳，模拟器自己和控制面板照常连着，按钮才能再把它接回来。
MQTT_PROXY_PORT = 3335
# 假机械臂的串口（pty 的符号链接），对应终端配置 controller_serial。
ARM_SERIAL_PATH = os.environ.get("RM_ARM_SERIAL", "/tmp/rm_terminal_arm")
