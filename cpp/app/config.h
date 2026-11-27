#pragma once
#include "logging.h"
#include <QString>
#include <QStringList>

namespace rm_terminal
{
    constexpr int kExitSuccess = 0;         // 成功
    constexpr int kExitBadArguments = 2;    // 命令行参数写错
    constexpr int kExitStartFailure = 3;    // 传输启动失败
    constexpr int kExitConfigError = 4;     // 配置或日志文件失败
    constexpr int kExitEvidenceFailure = 5; // 取证失败
    constexpr const char* kDefaultConfigFileName = "rm_terminal.conf";

    struct Config
    {
        QString mqtt_host = QStringLiteral("127.0.0.1");
        QString log_destination = QStringLiteral("rm_terminal.log");
        int mqtt_port = 3333;
        int udp_port = 3334;
        QString video_hwaccel = QStringLiteral("none"); // none = 软件解码;vaapi = Intel 核显硬件解码
        int self_robot_id = 3;
        bool self_robot_id_set = false;     // 配置文件里是否显式写了 self_robot_id。没写就要弹选席位页
        int stale_window_ms = 2200;         // 两帧加200ms余量算数据过期，字段频率1Hz
        int mode_exit_hysteresis_ms = 3000; // 致盲解除后等待时间
        int event_history_capacity = 50;
        int blind_stale_fallback_ms = 15000; // 致盲结束后的切回战术地图的保险
        LogLevel log_level = LogLevel::info;
        QString controller_serial;
        int controller_baud = 115200;
        int mqtt_keepalive_s = 5;   // 保活发现时长
        int alert_control_gap = 30; // 控制度落后多少分提醒一次。
        QString speech_dir;
        int map_mark_ms = 30000; // 指挥手给队友发的地图标注
        bool display_fullscreen = false;
        QString display_screen;
        QString video_relay_host;
        int video_wall_port = 3340;
    };

    const QStringList& alert_kind_names();
    bool load_config(const QString& path, Config* out, QString* error);
    bool valid_self_robot_id(int id);
    bool parse_log_level(const QString& name, LogLevel* out);
    bool valid_controller_baud(int baud);
    bool valid_ipv4(const QString& text);

} // namespace rm_terminal
