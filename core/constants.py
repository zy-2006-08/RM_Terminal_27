"""全局常量与协议参数。

数值来源 RoboMaster 2026 通信协议 V2.0.0（20260626）：
  - 服务器 IP 192.168.12.1，MQTT 端口 3333，自定义客户端 IP 192.168.12.2
  - 图传码流：UDP 3334，1920x1080，60fps，HEVC(H.265)，无重传
  - 每个 UDP 包最大 1400 字节，其中前 8 字节为大端头，数据部分 1392 字节
RM2027 正式协议尚未发布，本地模拟统一走 127.0.0.1。
"""

# ---------------- 网络 ----------------
# 真实比赛（2026）为 192.168.12.1 / .2，本地模拟用回环
SERVER_HOST = "127.0.0.1"
MQTT_PORT = 3333          # 2026 协议规定的 MQTT 端口
VIDEO_UDP_PORT = 3334     # 2026 协议规定的图传码流 UDP 端口

# ---------------- 图传 ----------------
VIDEO_WIDTH = 1920
VIDEO_HEIGHT = 1080
VIDEO_FPS = 60
UDP_PACKET_MAX = 1400     # 单个 UDP 包上限
UDP_HEADER_SIZE = 8       # 帧编号2 + 分片序号2 + 帧总字节数4，大端
UDP_PAYLOAD_MAX = UDP_PACKET_MAX - UDP_HEADER_SIZE   # 1392

# 重组器参数
FRAME_ASSEMBLY_TIMEOUT_S = 0.30   # 超过此时间未收齐则丢弃该帧
FRAME_CACHE_MAX = 8               # 同时在装配中的帧数上限，防止内存增长

# ---------------- 本机机器人身份 ----------------
# 附录二 ID 编号：1红英雄 2红工程 3/4/5红步兵 6红空中 7红哨兵
SELF_ROBOT_ID = 3         # 红方 3 号步兵
SELF_ROBOT_NAME = "红方 3 号步兵"

# ---------------- MQTT topic（与 2026 协议一致，topic 即指令名）----------------
T_GAME_STATUS = "GameStatus"
T_ROBOT_DYNAMIC = "RobotDynamicStatus"
T_ROBOT_MODULE = "RobotModuleStatus"
T_ROBOT_POSITION = "RobotPosition"
T_EVENT = "Event"
T_TELEMETRY = "RobotTelemetry"        # 机器人 -> 终端（对应 0x0310）
T_CUSTOM_CONTROL = "CustomControl"    # 终端 -> 机器人（对应 0x0311）
T_COMMON_COMMAND = "CommonCommand"    # 终端 -> 服务器
T_BLIND_STATUS = "BlindStatus"
T_ROBOT_POSITION_SET = "RobotPositionSet"
T_ROBOT_HEALTH_SET = "RobotHealthSet"

SUB_TOPICS = [
    T_GAME_STATUS,
    T_ROBOT_DYNAMIC,
    T_ROBOT_MODULE,
    T_ROBOT_POSITION,
    T_EVENT,
    T_TELEMETRY,
    T_BLIND_STATUS,
    T_ROBOT_POSITION_SET,
    T_ROBOT_HEALTH_SET,
]

# ---------------- 比赛阶段（2026 协议 current_stage 枚举）----------------
GAME_STAGES = {
    0: "未开始比赛",
    1: "准备阶段",
    2: "十五秒自检",
    3: "五秒倒计时",
    4: "比赛中",
    5: "比赛结算中",
}

# ---------------- 模块状态（0 离线 / 1 在线 / 2 安装不规范视为离线）----------------
MODULE_STATE = {0: "离线", 1: "在线", 2: "不规范"}

MODULE_LABELS = [
    ("power_manager", "电源管理"),
    ("main_controller", "主控"),
    ("armor", "装甲板"),
    ("small_shooter", "17mm 发射"),
    ("big_shooter", "42mm 发射"),
    ("video_transmission", "图传"),
    ("capacitor", "超级电容"),
    ("uwb", "定位"),
    ("rfid", "RFID"),
    ("light_strip", "灯条"),
    ("laser_detection_module", "激光检测"),
]

# ---------------- 底盘 / 控制模式 ----------------
CHASSIS_MODES = {
    0: "停机",
    1: "手动驾驶",
    2: "底盘跟随云台",
    3: "小陀螺",
    4: "自瞄模式",
}

# ---------------- 终端 -> 机器人 自定义指令 ID ----------------
# 走 CustomControl（30 字节上限），实际比赛需按正式协议约定编码
CMD_SET_CHASSIS_MODE = 0x01
CMD_TOGGLE_AUTOAIM = 0x02
CMD_EMERGENCY_STOP = 0x03
CMD_RESET_GIMBAL = 0x04

# ---------------- 失效判定阈值 ----------------
TELEMETRY_TIMEOUT_S = 0.5     # 机器人遥测心跳超时
VIDEO_TIMEOUT_S = 1.0         # 图传中断判定
VISION_TIMEOUT_S = 0.3        # 视觉目标丢失后降级
