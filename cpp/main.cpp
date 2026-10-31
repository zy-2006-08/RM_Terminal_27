#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QScreen>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QString>
#include <QStringList>
#include <QKeySequence>
#include <QShortcut>
#include <QTimer>
#include <memory>
#include <csignal>
#include <cstdio>
#include <optional>
#include <string>

#include <signal.h>

#include "clock.h"
#include "config.h"
#include "alert_voice.h"
#include "controller_link.h"
#include "dashboard.h"
#include "link_health.h"
#include "match_alerts.h"
#include "rm_terminal.pb.h"
#include "logging.h"
#include "mqtt_intake.h"
#include "platform.h"
#include "presentation.h"
#include "recorder.h"
#include "reminder_repository.h"
#include "replay_player.h"
#include "seat.h"
#include "seat_picker.h"
#include "assembly_pane.h"
#include "dart_pane.h"
#include "km_control.h"
#include "revive_popup.h"
#include "self_command_pane.h"
#include "sentry_pane.h"
#include "store.h"
#include "tactical_reminder_controller.h"
#include "ui_mode.h"
#include "video_receiver.h"

#if defined(RM_TERMINAL_HAS_SPEECH)
#include "macos_speech_backend.h"
#elif defined(RM_TERMINAL_PIPER_SPEECH)
#include "piper_speech_backend.h"
#endif

namespace {

// 席位从哪来:--seat > RM_SELF_ID > 配置里显式写的 self_robot_id;都没有时返回空,
// 由界面弹选席位页(回放则用录像里记的席位)。
bool prepare_runtime(const QString& config_path, rm_terminal::Config* cfg,
                     std::optional<int> cli_seat = std::nullopt, QString* seat_source = nullptr) {
    QString error;
    if (!rm_terminal::load_config(config_path, cfg, &error)) {
        fprintf(stderr, "config error: %s\n", error.toUtf8().constData());
        return false;
    }
    QString source = cfg->self_robot_id_set ? QStringLiteral("config") : QString();
    const QString self_id = qEnvironmentVariable("RM_SELF_ID");
    if (!self_id.isEmpty()) {
        bool ok = false;
        const int id = self_id.toInt(&ok);
        if (!ok || !rm_terminal::valid_self_robot_id(id)) {
            fprintf(stderr,
                    "config error: invalid RM_SELF_ID=%s; expected 1, 3, 4, 6, "
                    "101, 103, 104, or 106\n",
                    self_id.toUtf8().constData());
            return false;
        }
        cfg->self_robot_id = id;
        source = QStringLiteral("env");
    }
    if (cli_seat) {
        cfg->self_robot_id = *cli_seat;
        source = QStringLiteral("cli");
    }
    if (seat_source) *seat_source = source;
    if (!rm_terminal::StructuredLog::open(cfg->log_destination, cfg->log_level, &error)) {
        fprintf(stderr, "log error: %s\n", error.toUtf8().constData());
        return false;
    }
    rm_terminal::set_log_sink([](rm_terminal::LogLevel level, const QString& event,
                                 const QStringList& fields) {
        rm_terminal::StructuredLog::write(level, event, fields);
    });
    return true;
}

// Ctrl-C/kill 的默认动作是直接终止进程,析构函数不跑,录像最后一条会只写一半。
// 信号处理函数里只能做异步信号安全的事,所以只记下信号号,由主线程定时器去 quit。
// SA_RESETHAND:第二次 Ctrl-C 恢复默认动作,卡死时仍能强行结束。
volatile std::sig_atomic_t g_quit_signal = 0;

extern "C" void remember_quit_signal(int signal_number) { g_quit_signal = signal_number; }

void install_quit_signals() {
    struct sigaction action {};
    action.sa_handler = remember_quit_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESETHAND;
    for (const int signal_number : {SIGINT, SIGTERM, SIGHUP})
        sigaction(signal_number, &action, nullptr);
}

void log_shutdown(const QString& mode, int code) {
    rm_terminal::StructuredLog::write(
        rm_terminal::LogLevel::info, QStringLiteral("shutdown"),
        {QStringLiteral("mode=%1").arg(mode), QStringLiteral("exit=%1").arg(code)});
    rm_terminal::set_log_sink(nullptr);
    rm_terminal::StructuredLog::close();
}

// 语音的平台边界收在这一个函数里,上层不再有 #if。
rm_terminal::SpeechBackend* make_speech_backend(QObject* parent, const QString& speech_dir) {
#if defined(RM_TERMINAL_HAS_SPEECH)
    Q_UNUSED(speech_dir);
    return new rm_terminal::MacosSpeechBackend(parent);
#elif defined(RM_TERMINAL_PIPER_SPEECH)
    return new rm_terminal::PiperSpeechBackend(
        speech_dir, QStringLiteral(RM_TERMINAL_SOURCE_DIR "/scripts/tts_say.py"), parent);
#else
    Q_UNUSED(speech_dir);
    return new rm_terminal::UnavailableSpeechBackend(
        QStringLiteral("当前平台没有本机中文离线语音，提醒只有横幅"), parent);
#endif
}

QStringList startup_fields(const QString& mode, const rm_terminal::Config& cfg,
                           const QString& seat_source = QString()) {
    const auto seat = rm_terminal::Seat::fromRobotId(cfg.self_robot_id);
    return {QStringLiteral("mode=%1").arg(mode),
            QStringLiteral("seat=%1").arg(cfg.self_robot_id),
            QStringLiteral("role=%1").arg(seat ? rm_terminal::role_name(seat->role()) : QStringLiteral("none")),
            QStringLiteral("source=%1").arg(seat_source.isEmpty() ? QStringLiteral("default") : seat_source),
            QStringLiteral("platform=%1").arg(QString::fromLatin1(rm_terminal::platform_name())),
            QStringLiteral("mqtt_host=%1").arg(cfg.mqtt_host),
            QStringLiteral("mqtt_port=%1").arg(cfg.mqtt_port),
            QStringLiteral("udp_port=%1").arg(cfg.udp_port),
            QStringLiteral("self_robot_id=%1").arg(cfg.self_robot_id),
            QStringLiteral("stale_window_ms=%1").arg(cfg.stale_window_ms)};
}

}

int main(int argc, char* argv[]) {
    QStringList args;
    for (int index = 1; index < argc; ++index) args.append(QString::fromLocal8Bit(argv[index]));

    QString config_path = qEnvironmentVariable(
        "RM_TERMINAL_CONFIG", QString::fromLatin1(rm_terminal::kDefaultConfigFileName));
    const int config_at = args.indexOf(QLatin1String("--config"));
    if (config_at >= 0) {
        if (config_at + 1 >= args.size()) {
            qCritical().noquote() << "Missing path after --config";
            return rm_terminal::kExitBadArguments;
        }
        config_path = args.at(config_at + 1);
        args.removeAt(config_at + 1);
        args.removeAt(config_at);
    }
    if (args.contains(QLatin1String("--config"))) {
        qCritical().noquote() << "Duplicate --config is not supported";
        return rm_terminal::kExitBadArguments;
    }

    QString screenshot_path;
    const int screenshot_at = args.indexOf(QLatin1String("--screenshot"));
    if (screenshot_at >= 0) {
        if (screenshot_at + 1 >= args.size()) {
            qCritical().noquote() << "Missing path after --screenshot";
            return rm_terminal::kExitBadArguments;
        }
        screenshot_path = args.at(screenshot_at + 1);
        args.removeAt(screenshot_at + 1);
        args.removeAt(screenshot_at);
    }
    if (args.contains(QLatin1String("--screenshot"))) {
        qCritical().noquote() << "Duplicate --screenshot is not supported";
        return rm_terminal::kExitBadArguments;
    }

    QString dump_layout_path;
    const int dump_layout_at = args.indexOf(QLatin1String("--dump-layout"));
    if (dump_layout_at >= 0) {
        if (dump_layout_at + 1 >= args.size()) {
            qCritical().noquote() << "Missing path after --dump-layout";
            return rm_terminal::kExitBadArguments;
        }
        dump_layout_path = args.at(dump_layout_at + 1);
        args.removeAt(dump_layout_at + 1);
        args.removeAt(dump_layout_at);
    }
    if (args.contains(QLatin1String("--dump-layout"))) {
        qCritical().noquote() << "Duplicate --dump-layout is not supported";
        return rm_terminal::kExitBadArguments;
    }

    std::optional<rm_terminal::UiMode> forced_mode;
    const int force_mode_at = args.indexOf(QLatin1String("--force-mode"));
    if (force_mode_at >= 0) {
        if (force_mode_at + 1 >= args.size()) {
            qCritical().noquote() << "Missing mode after --force-mode; expected info or video";
            return rm_terminal::kExitBadArguments;
        }
        const QString requested = args.at(force_mode_at + 1);
        if (requested == QLatin1String("info")) {
            forced_mode = rm_terminal::UiMode::Info;
        } else if (requested == QLatin1String("video")) {
            forced_mode = rm_terminal::UiMode::Video;
        } else {
            qCritical().noquote() << "Invalid --force-mode value; expected info or video, got"
                                  << requested;
            return rm_terminal::kExitBadArguments;
        }
        args.removeAt(force_mode_at + 1);
        args.removeAt(force_mode_at);
    }
    if (args.contains(QLatin1String("--force-mode"))) {
        qCritical().noquote() << "Duplicate --force-mode is not supported";
        return rm_terminal::kExitBadArguments;
    }

    std::optional<int> cli_seat;
    const int seat_at = args.indexOf(QLatin1String("--seat"));
    if (seat_at >= 0) {
        if (seat_at + 1 >= args.size()) {
            qCritical().noquote()
                << "Missing seat after --seat; expected 1, 3, 4, 6, 101, 103, 104 or 106";
            return rm_terminal::kExitBadArguments;
        }
        bool ok = false;
        const int id = args.at(seat_at + 1).toInt(&ok);
        if (!ok || !rm_terminal::Seat::fromRobotId(id)) {
            qCritical().noquote() << "Invalid --seat value; expected 1, 3, 4, 6, 101, 103, 104 or 106, got"
                                  << args.at(seat_at + 1);
            return rm_terminal::kExitBadArguments;
        }
        cli_seat = id;
        args.removeAt(seat_at + 1);
        args.removeAt(seat_at);
    }
    if (args.contains(QLatin1String("--seat"))) {
        qCritical().noquote() << "Duplicate --seat is not supported";
        return rm_terminal::kExitBadArguments;
    }

    QString replay_path;
    const int replay_at = args.indexOf(QLatin1String("--replay"));
    if (replay_at >= 0) {
        if (replay_at + 1 >= args.size()) {
            qCritical().noquote() << "Missing path after --replay";
            return rm_terminal::kExitBadArguments;
        }
        replay_path = args.at(replay_at + 1);
        args.removeAt(replay_at + 1);
        args.removeAt(replay_at);
    }
    if (args.contains(QLatin1String("--replay"))) {
        qCritical().noquote() << "Duplicate --replay is not supported";
        return rm_terminal::kExitBadArguments;
    }

    // 测试会拉起真实程序,默认自动录制曾把验收产物写进用户录像目录。
    const bool no_record = args.removeOne(QLatin1String("--no-record"));

    const bool safe_smoke = args.size() == 1 && args.at(0) == QLatin1String("--safe-smoke");
    const bool diagnostic = args.size() == 4 && args.at(0) == QLatin1String("--diagnostic");

    // Rejected rather than ignored. Both of these modes are headless, so there is
    // no layout to force; and because the flag was stripped above, staying silent
    // would let an evidence-capture command appear to succeed while proving nothing.
    if (forced_mode.has_value() && (safe_smoke || diagnostic)) {
        qCritical().noquote()
            << "--force-mode requires the GUI; it cannot combine with --safe-smoke or --diagnostic";
        return rm_terminal::kExitBadArguments;
    }
    if (!dump_layout_path.isEmpty() && (safe_smoke || diagnostic)) {
        qCritical().noquote() << "--dump-layout requires the GUI; it cannot combine with"
                                 " --safe-smoke or --diagnostic";
        return rm_terminal::kExitBadArguments;
    }
    if (!replay_path.isEmpty() && (safe_smoke || diagnostic)) {
        qCritical().noquote() << "--replay requires the GUI; it cannot combine with"
                                 " --safe-smoke or --diagnostic";
        return rm_terminal::kExitBadArguments;
    }

    if (safe_smoke) {
        QCoreApplication app(argc, argv);
        qInfo().noquote() << "RM terminal: safe smoke; platform="
                          << rm_terminal::platform_name();
        rm_terminal::Config cfg;
        QString seat_source;
        if (!prepare_runtime(config_path, &cfg, cli_seat, &seat_source))
            return rm_terminal::kExitConfigError;
        rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("startup"),
                                          startup_fields(QStringLiteral("safe_smoke"), cfg, seat_source));
        log_shutdown(QStringLiteral("safe_smoke"), rm_terminal::kExitSuccess);
        return rm_terminal::kExitSuccess;
    }

    if (diagnostic) {
        QCoreApplication app(argc, argv);
        bool port_ok = false;
        bool seconds_ok = false;
        const int port = args.at(2).toInt(&port_ok);
        const int seconds = args.at(3).toInt(&seconds_ok);
        if (!port_ok || !seconds_ok || port <= 0 || seconds <= 0 || seconds > 30) {
            return rm_terminal::kExitBadArguments;
        }
        rm_terminal::Config cfg;
        QString seat_source;
        if (!prepare_runtime(config_path, &cfg, cli_seat, &seat_source))
            return rm_terminal::kExitConfigError;
        rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("startup"),
                                          startup_fields(QStringLiteral("diagnostic"), cfg, seat_source));
        rm_terminal::Store store(cfg.stale_window_ms);
        rm_terminal::MqttIntake intake(store, args.at(1), port, cfg.self_robot_id);
        QObject::connect(&intake, &rm_terminal::MqttIntake::diagnostic,
                         [](const QString& text) { qInfo().noquote() << text; });
        if (!intake.start()) {
            rm_terminal::StructuredLog::write(
                rm_terminal::LogLevel::error, QStringLiteral("reconnect_mqtt"),
                {QStringLiteral("state=start_failed"), QStringLiteral("host=%1").arg(args.at(1))});
            log_shutdown(QStringLiteral("diagnostic"), rm_terminal::kExitStartFailure);
            return rm_terminal::kExitStartFailure;
        }
        QTimer::singleShot(seconds * 1000, &app, &QCoreApplication::quit);
        const int result = app.exec();
        const auto snapshot = store.snapshot(rm_terminal::monotonic_now());
        qInfo().noquote() << "diagnostic snapshot:" << snapshot.robots.size()
                          << "robot(s); game.stage=" << static_cast<int>(snapshot.game.current_stage.quality)
                          << "events=" << static_cast<qulonglong>(snapshot.events.size());
        log_shutdown(QStringLiteral("diagnostic"), result);
        return result;
    }

    if (!args.isEmpty()) {
        qCritical().noquote() << "Unknown argument; supported: --safe-smoke, --config <path>,"
                                 " --seat <id>, --screenshot <path>, --force-mode <info|video>,"
                                 " --replay <path>, --dump-layout <path>, --no-record,"
                                 " --diagnostic <host> <port> <seconds>";
        return rm_terminal::kExitBadArguments;
    }

    QApplication app(argc, argv);
    rm_terminal::Config cfg;
    QString seat_source;
    if (!prepare_runtime(config_path, &cfg, cli_seat, &seat_source))
        return rm_terminal::kExitConfigError;
    // 席位在连网和建主界面之前定下来:登录名(MQTT clientID)和布局都取决于它,
    // 本次运行内不能再改。回放用录像里记的席位,不弹选席位页。
    if (seat_source.isEmpty() && replay_path.isEmpty()) {
        rm_terminal::SeatPicker picker;
        const auto seat = picker.exec();
        if (!seat) {
            rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("seat_cancelled"), {});
            log_shutdown(QStringLiteral("terminal"), rm_terminal::kExitSuccess);
            return rm_terminal::kExitSuccess;
        }
        cfg.self_robot_id = static_cast<int>(seat->robotId());
        seat_source = QStringLiteral("picker");
    }
    if (replay_path.isEmpty())
        rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("startup"),
                                          startup_fields(QStringLiteral("terminal"), cfg, seat_source));

    install_quit_signals();
    QTimer quit_signal_poll;
    QObject::connect(&quit_signal_poll, &QTimer::timeout, &app, [&app]() {
        if (g_quit_signal == 0) return;
        rm_terminal::StructuredLog::write(
            rm_terminal::LogLevel::info, QStringLiteral("quit_signal"),
            {QStringLiteral("signal=%1").arg(static_cast<int>(g_quit_signal))});
        g_quit_signal = 0;
        app.quit();
    });
    quit_signal_poll.start(100);

    rm_terminal::Store store(cfg.stale_window_ms);
    rm_terminal::MqttIntake intake(store, cfg.mqtt_host, cfg.mqtt_port,
                                   cfg.self_robot_id);
    QObject::connect(&intake, &rm_terminal::MqttIntake::diagnostic,
                     [](const QString& text) { qInfo().noquote() << text; });

    rm_terminal::Recorder recorder;
    intake.setMessageObserver([&recorder](const QString& topic, const QByteArray& payload) {
        recorder.recordMqtt(topic, payload);
    });

    const QString recordings =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("recordings"));
    if (replay_path.isEmpty() && !no_record) {
        rm_terminal::RecordingMetadata metadata;
        metadata.self_robot_id = cfg.self_robot_id;
        metadata.mqtt_host = cfg.mqtt_host;
        metadata.mqtt_port = cfg.mqtt_port;
        metadata.udp_port = cfg.udp_port;
        QString record_error;
        if (!recorder.start(recordings, metadata, &record_error)) {
            qWarning().noquote() << "recording unavailable:" << record_error;
        } else {
            qInfo().noquote() << "recording:" << recorder.path();
        }
    }

    intake.setKeepAlive(cfg.mqtt_keepalive_s);
    if (replay_path.isEmpty() && !intake.start()) {
        rm_terminal::StructuredLog::write(
            rm_terminal::LogLevel::warning, QStringLiteral("reconnect_mqtt"),
            {QStringLiteral("state=unavailable"),
             QStringLiteral("host=%1").arg(cfg.mqtt_host),
             QStringLiteral("port=%1").arg(cfg.mqtt_port)});
    }

    rm_terminal::VideoReceiver video;
    video.setDatagramObserver([&recorder](const QByteArray& datagram) {
        recorder.recordVideo(datagram);
    });

    std::unique_ptr<rm_terminal::ReplayPlayer> replay;
    if (!replay_path.isEmpty()) {
        if (!video.startInjected()) {
            rm_terminal::StructuredLog::write(
                rm_terminal::LogLevel::warning, QStringLiteral("reconnect_udp"),
                {QStringLiteral("state=replay_decoder_failed")});
        }
        replay = std::make_unique<rm_terminal::ReplayPlayer>(store, video);
        QString replay_error;
        if (!replay->start(replay_path, &replay_error)) {
            qCritical().noquote() << "replay failed:" << replay_error;
            rm_terminal::StructuredLog::write(
                rm_terminal::LogLevel::error, QStringLiteral("replay"),
                {QStringLiteral("state=open_failed"),
                 QStringLiteral("path=%1").arg(replay_path)});
            log_shutdown(QStringLiteral("replay"), rm_terminal::kExitStartFailure);
            return rm_terminal::kExitStartFailure;
        }
        qInfo().noquote() << "replaying:" << replay_path;
        if (seat_source.isEmpty() && rm_terminal::Seat::fromRobotId(replay->selfRobotId())) {
            cfg.self_robot_id = replay->selfRobotId();
            seat_source = QStringLiteral("recording");
        }
        rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("startup"),
                                          startup_fields(QStringLiteral("replay"), cfg, seat_source));
    } else {
        rm_terminal::VideoEndpoint endpoint;
        endpoint.port = static_cast<quint16>(cfg.udp_port);
        if (!video.start(endpoint)) {
            rm_terminal::StructuredLog::write(
                rm_terminal::LogLevel::warning, QStringLiteral("reconnect_udp"),
                {QStringLiteral("state=start_failed"),
                 QStringLiteral("port=%1").arg(cfg.udp_port)});
        }
    }

    // 机械臂 -> 串口 -> 这里 -> MQTT CustomControl -> 车。回放时不开串口:看录像不该
    // 让桌上的机械臂数据被发出去。每帧只在收到时转发一次,断线或发不出去就丢掉,
    // 绝不补发 —— 补发的是旧姿态,车上的机械臂会去追一个早已过去的位置。
    rm_terminal::ControllerLink controller(replay ? QString() : cfg.controller_serial,
                                           cfg.controller_baud);
    std::optional<rm_terminal::MonotonicMs> last_forward_at;
    QObject::connect(&controller, &rm_terminal::ControllerLink::frameReceived, &app,
                     [&intake, &recorder, &last_forward_at](const QByteArray& data) {
                         // 2026 协议:CustomControl 最高 75Hz。固件发 25Hz,这里只防
                         // 接错设备时把服务器打爆。
                         constexpr rm_terminal::MonotonicMs kMinIntervalMs = 14;
                         const rm_terminal::MonotonicMs now = rm_terminal::monotonic_now();
                         if (last_forward_at && now - *last_forward_at < kMinIntervalMs) return;
                         rm::CustomControl message;
                         message.set_data(data.constData(), static_cast<std::size_t>(data.size()));
                         const std::string bytes = message.SerializeAsString();
                         if (!intake.publish("CustomControl", bytes)) return;
                         last_forward_at = now;
                         // 发出去的也录下来:复盘时能看到当时给车下了什么姿态。回放的
                         // 解码器不认这个 topic,会直接跳过。
                         recorder.recordMqtt(QStringLiteral("CustomControl"),
                                             QByteArray::fromStdString(bytes));
                     });
    // 反方向:车经 0x0310 回传的机械臂姿态,转成控制器认的 0x0309 帧写回串口。同样不排队、
    // 不补发,串口没开就丢。
    std::uint8_t feedback_seq = 0;
    std::optional<rm_terminal::MonotonicMs> last_feedback_at;
    bool feedback_invalid = false;
    QObject::connect(&intake, &rm_terminal::MqttIntake::customBytes, &app,
                     [&controller, &feedback_seq, &last_feedback_at,
                      &feedback_invalid](const QByteArray& data) {
                         // 0x0310 最高 50Hz,这里只防车端程序写错时把串口打满。
                         constexpr rm_terminal::MonotonicMs kMinIntervalMs = 15;
                         const rm_terminal::MonotonicMs now = rm_terminal::monotonic_now();
                         if (last_feedback_at && now - *last_feedback_at < kMinIntervalMs) return;
                         const auto frame = rm_terminal::arm_feedback_frame(data, feedback_seq);
                         if (!frame) {
                             if (!feedback_invalid)
                                 rm_terminal::StructuredLog::write(
                                     rm_terminal::LogLevel::warning, QStringLiteral("arm_feedback"),
                                     {QStringLiteral("state=invalid"),
                                      QStringLiteral("bytes=%1").arg(data.size())});
                             feedback_invalid = true;
                             return;
                         }
                         feedback_invalid = false;
                         if (!controller.send(*frame)) return;
                         ++feedback_seq;
                         last_feedback_at = now;
                     });
    controller.start();

    // 语音后端挂在 app 上,生命周期覆盖整个进程:控制器只借用它,不拥有它。
    auto* speech = make_speech_backend(&app, cfg.speech_dir);
    rm_terminal::ReminderController reminders(
        std::make_unique<rm_terminal::ReminderRepository>(
            rm_terminal::ReminderRepository::defaultDirectory()),
        speech, cfg.stale_window_ms);
    // 音频缓存和配置分开放:缓存是可再生的派生物,清掉只会重新合成;配置是
    // 操作手赛前填的战术,清掉就没了。混在一个目录里迟早会被一起删。
    const auto cache_dir =
        QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
            .filePath(QStringLiteral("reminders"));
    if (const auto error = reminders.start(cache_dir); !error.isEmpty())
        rm_terminal::StructuredLog::write(rm_terminal::LogLevel::warning,
                                          QStringLiteral("reminder_config"),
                                          {QStringLiteral("state=invalid"),
                                           QStringLiteral("error=%1").arg(error)});

    // 事件提醒:判定是纯逻辑,语音用独立后端赛前合成好,播放和战术提醒共用一个喇叭。
    // 回放时照样出横幅(复盘时能看到当时提醒了什么),但不出声。
    rm_terminal::MatchAlertSettings alert_settings;
    alert_settings.self_faction = cfg.self_robot_id > 100 ? 2u : 1u;
    alert_settings.control_gap = cfg.alert_control_gap;
    alert_settings.disabled = cfg.alerts_disabled;
    rm_terminal::MatchAlerts match_alerts(alert_settings);
    rm_terminal::AlertVoice alert_voice(make_speech_backend(&app, cfg.speech_dir));
    if (!replay)
        alert_voice.prepare(cache_dir, rm_terminal::match_alert_phrases(), reminders.currentVoice(),
                            reminders.config().rate_wpm);

    rm_terminal::Dashboard dashboard(cfg);
    dashboard.attachReminders(&reminders);
    QObject::connect(&reminders, &rm_terminal::ReminderController::reminderDue, &dashboard,
                     [&dashboard](const QString& text) {
                         rm_terminal::StructuredLog::write(
                             rm_terminal::LogLevel::info, QStringLiteral("alert"),
                             {QStringLiteral("kind=tactical"), QStringLiteral("level=info"),
                              QStringLiteral("text=%1").arg(text)});
                         dashboard.showAlert(text, rm_terminal::AlertLevel::Info,
                                             rm_terminal::monotonic_now());
                     });
    dashboard.setWindowTitle(QStringLiteral("RM Terminal · %1")
                                 .arg(rm_terminal::Seat::fromRobotId(cfg.self_robot_id)
                                          ? rm_terminal::Seat::fromRobotId(cfg.self_robot_id)->label()
                                          : QString::number(cfg.self_robot_id)));
    // Four columns plus the flanking roster cards need more width than the old
    // two-column layout; below this the map degrades to a sliver.
    dashboard.setMinimumSize(1280, 760);
    // 离屏截图默认 1440x900;RM_TERMINAL_CAPTURE_SIZE=1920x1080 按官方显示器出图。
    QSize window_size(1440, 900);
    if (const QString wanted = qEnvironmentVariable("RM_TERMINAL_CAPTURE_SIZE"); !wanted.isEmpty()) {
        const QStringList parts = wanted.split(QLatin1Char('x'));
        bool w_ok = false;
        bool h_ok = false;
        const int w = parts.size() == 2 ? parts[0].toInt(&w_ok) : 0;
        const int h = parts.size() == 2 ? parts[1].toInt(&h_ok) : 0;
        if (w_ok && h_ok && w >= 1280 && h >= 760) window_size = QSize(w, h);
        else qWarning().noquote() << "ignoring RM_TERMINAL_CAPTURE_SIZE=" << wanted;
    }
    dashboard.resize(window_size);
    // 屏幕清单写进日志:display_screen 要填这里的名字。
    QStringList screen_names;
    for (const QScreen* screen : QGuiApplication::screens())
        screen_names << QStringLiteral("%1:%2x%3").arg(screen->name()).arg(screen->geometry().width())
                            .arg(screen->geometry().height());
    rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("screens"),
                                      {QStringLiteral("names=%1").arg(screen_names.join(QLatin1Char(',')))});
    bool fullscreen = false;
    // 有些屏幕的可用高度装不下 900 的窗口，底部的回放控制条会被挤出屏幕。
    // 实跑时按可用区域收一点，离屏截图仍保持固定尺寸。
    if (QGuiApplication::platformName() != QLatin1String("offscreen")) {
        const QScreen* target = QGuiApplication::primaryScreen();
        if (cfg.display_fullscreen && !cfg.display_screen.isEmpty()) {
            target = nullptr;
            for (QScreen* screen : QGuiApplication::screens())
                if (screen->name() == cfg.display_screen) target = screen;
            if (!target) {
                rm_terminal::StructuredLog::write(
                    rm_terminal::LogLevel::warning, QStringLiteral("display"),
                    {QStringLiteral("state=screen_missing"), QStringLiteral("wanted=%1").arg(cfg.display_screen)});
                target = QGuiApplication::primaryScreen();
            }
        }
        if (target && cfg.display_fullscreen) {
            dashboard.setGeometry(target->geometry());
            fullscreen = true;
        } else if (target) {
            const QRect available = target->availableGeometry();
            const int width = std::min(1440, available.width());
            const int height = std::clamp(available.height() - 60, 760, 900);
            dashboard.resize(width, height);
            dashboard.move(available.x() + (available.width() - width) / 2,
                           available.y() + 30);
        }
    }
    // Applied before the first update() so the forced layout holds from the very
    // first tick; injecting it afterwards would render one automatic frame first
    // and put the wrong layout in a capture taken at a short delay.
    if (forced_mode.has_value()) dashboard.forceMode(forced_mode);
    const rm_terminal::MonotonicMs startup_now = rm_terminal::monotonic_now();
    dashboard.update(store.snapshot(startup_now), &video, startup_now);
    dashboard.setRecordingStatus(replay ? replay->statusText() : recorder.statusText());
    if (replay) {
        dashboard.showReplayControls(true);
        dashboard.setReplayPaused(replay->paused());
        dashboard.setReplayProgress(replay->positionMs(), replay->durationMs());
        dashboard.setReplaySeekHandler([&replay](qint64 position_ms) { replay->seek(position_ms); });
        dashboard.setReplayPauseHandler([&replay] { replay->togglePause(); });
        dashboard.setReplaySpeedHandler([&replay](double speed) { replay->setSpeed(speed); });
        QObject::connect(replay.get(), &rm_terminal::ReplayPlayer::progress, &dashboard,
                         &rm_terminal::Dashboard::setReplayProgress);
        QObject::connect(replay.get(), &rm_terminal::ReplayPlayer::pausedChanged, &dashboard,
                         &rm_terminal::Dashboard::setReplayPaused);
    }
    // 角色面板放进布局给的卡片里。回放时不发任何指令:intake 没启动,publish 必然返回假。
    const auto send = [&intake, &replay](const char* topic, const std::string& payload) {
        return !replay && intake.publish(topic, payload);
    };
    dashboard.setCommandSender(send);
    rm_terminal::AssemblyPane* assembly = nullptr;
    rm_terminal::DartPane* dart = nullptr;
    rm_terminal::SentryPane* sentry = nullptr;
    if (QWidget* card = dashboard.paneCard(rm_terminal::PaneId::Assembly)) {
        assembly = new rm_terminal::AssemblyPane(card);
        card->layout()->addWidget(assembly);
        assembly->setSender(send);
    }
    if (QWidget* card = dashboard.paneCard(rm_terminal::PaneId::Dart)) {
        dart = new rm_terminal::DartPane(card);
        card->layout()->addWidget(dart);
        dart->setSender(send);
    }
    if (QWidget* card = dashboard.paneCard(rm_terminal::PaneId::Sentry)) {
        sentry = new rm_terminal::SentryPane(card);
        card->layout()->addWidget(sentry);
        sentry->setSender(send);
    }
    rm_terminal::SelfCommandPane* self_commands = nullptr;
    if (QWidget* card = dashboard.paneCard(rm_terminal::PaneId::SelfCommands)) {
        self_commands = new rm_terminal::SelfCommandPane(card);
        card->layout()->addWidget(self_commands);
        self_commands->setSender(send);
    }
    // 复活框照官方选手端:阵亡才弹出;读条结束后按 Y(或点「免费复活」)确认,点「金币复活」
    // 花钱。活着时按 Y 什么都不发。
    rm_terminal::RevivePopup* revive_popup = nullptr;
    if (self_commands) {
        revive_popup = new rm_terminal::RevivePopup(&dashboard);
        dashboard.setEliminatedOverlayReplaced(true);
        revive_popup->setActions([self_commands] { self_commands->confirmRevive(); },
                                 [self_commands] { self_commands->payRevive(); });
        auto* revive_key = new QShortcut(QKeySequence(Qt::Key_Y), &dashboard);
        revive_key->setContext(Qt::ApplicationShortcut);
        QObject::connect(revive_key, &QShortcut::activated, self_commands,
                         [self_commands] { self_commands->confirmRevive(); });
    }
    // 键鼠控制只在实况下有:回放没有链路,录像里也没有这台电脑的键盘。
    std::unique_ptr<rm_terminal::KmCapture> keyboard_mouse;
    if (self_commands && !replay) {
        keyboard_mouse = std::make_unique<rm_terminal::KmCapture>(&dashboard);
        rm_terminal::KmCapture* km = keyboard_mouse.get();
        km->setSender(send);
        dashboard.setKeyboardMouseToggle([km] { km->toggle(); });
        auto* toggle_key = new QShortcut(QKeySequence(Qt::Key_F9), &dashboard);
        toggle_key->setContext(Qt::ApplicationShortcut);
        QObject::connect(toggle_key, &QShortcut::activated, km, [km] { km->toggle(); });
        QObject::connect(km, &rm_terminal::KmCapture::activeChanged, &dashboard,
                         [km, self_commands, &dashboard](bool active, const QString& reason) {
                             rm_terminal::StructuredLog::write(
                                 rm_terminal::LogLevel::info, QStringLiteral("km_capture"),
                                 {QStringLiteral("state=%1").arg(active ? "on" : "off"),
                                  QStringLiteral("reason=%1").arg(reason.isEmpty() ? QStringLiteral("-") : reason),
                                  QStringLiteral("frames=%1").arg(km->framesSent()),
                                  QStringLiteral("pointer_lock=%1").arg(km->pointerLock() ? 1 : 0)});
                             self_commands->setKeyboardMouseState(active, QString());
                             dashboard.setKeyboardMouseStatus(
                                 active ? QStringLiteral("键鼠控制中（Esc 退出）") : QString());
                         });
        QObject::connect(km, &rm_terminal::KmCapture::suspendedChanged, &dashboard,
                         [km, &dashboard](bool paused) {
                             rm_terminal::StructuredLog::write(
                                 rm_terminal::LogLevel::info, QStringLiteral("km_capture"),
                                 {QStringLiteral("state=%1").arg(paused ? "paused" : "resumed"),
                                  QStringLiteral("reason=%1").arg(paused ? "阵亡" : "复活"),
                                  QStringLiteral("frames=%1").arg(km->framesSent())});
                             dashboard.setKeyboardMouseStatus(
                                 paused ? QStringLiteral("键鼠控制暂停（阵亡，复活后自动继续）")
                                        : QStringLiteral("键鼠控制中（Esc 退出）"));
                         });
    }

    if (fullscreen) dashboard.showFullScreen();
    else dashboard.show();
    // 从 Terminal / Finder 脚本拉起时进程没有前台权限，窗口会藏在别的窗口后面，
    // 看起来像「双击没反应」。raise + activateWindow 把它顶到最前。
    dashboard.raise();
    dashboard.activateWindow();

    rm_terminal::StaleReporter reporter;
    // 链路灯和自检页只在实况下有意义:回放时没有 MQTT 也没有串口,亮一排红灯只会误导。
    struct LinkMeters {
        rm_terminal::RateMeter referee, video, controller, feedback;
        rm_terminal::LinkStateLog log;
    } meters;
    const auto sample_links = [&cfg, &intake, &video, &controller, &meters](
                                  rm_terminal::MonotonicMs now) {
        const auto mqtt = intake.linkStats();
        rm_terminal::RefereeSample referee;
        referee.connected = mqtt.connected;
        referee.silent_ms = rm_terminal::silence(mqtt.last_message_at, mqtt.connected_at, now);
        referee.messages_per_s = meters.referee.update(mqtt.messages, now);

        rm_terminal::LinkReport report;
        report.referee = rm_terminal::referee_status(referee);

        // 2027 协议里车端唯一的上行数据是 0x0310(CustomByteBlock),按它最后一帧的时间判活。
        rm_terminal::CarSample car;
        car.referee_down = report.referee.level == rm_terminal::LinkLevel::Down;
        if (mqtt.last_custom_bytes_at) {
            car.freshness = now - *mqtt.last_custom_bytes_at < cfg.stale_window_ms
                                ? rm_terminal::Freshness::Fresh
                                : rm_terminal::Freshness::Stale;
            car.silent_ms = rm_terminal::silence(mqtt.last_custom_bytes_at, std::nullopt, now);
        }
        report.car = rm_terminal::car_status(car);

        rm_terminal::VideoSample picture;
        picture.online = video.online();
        if (video.msSinceLastFrame() >= 0) picture.silent_ms = video.msSinceLastFrame();
        picture.frames_per_s =
            meters.video.update(static_cast<std::uint64_t>(video.decodedFrames()), now);
        report.video = rm_terminal::video_status(picture);

        const auto arm = controller.stats();
        rm_terminal::ControllerSample sample;
        sample.configured = arm.configured;
        sample.open = arm.open;
        sample.error = arm.error;
        sample.silent_ms = rm_terminal::silence(arm.last_frame_at, std::nullopt, now);
        sample.frames_per_s = meters.controller.update(arm.frames, now);
        sample.bad_frames = arm.bad_frames;
        sample.joints = arm.joints;
        sample.can_forward = mqtt.connected;
        sample.feedback_per_s = meters.feedback.update(arm.sent, now);
        report.controller = rm_terminal::controller_status(sample);

        meters.log.observe(report);
        return report;
    };

    rm_terminal::RateMeter arm_rate;
    std::optional<bool> seat_matched;
    // 赛事事件和指挥手指令的回执也写进日志:复盘和验收脚本都靠它们看「服务器说了什么」。
    rm_terminal::MonotonicMs logged_event_at = -1;
    rm_terminal::MonotonicMs logged_sentry_result_at = -1;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> logged_dart;
    rm_terminal::MonotonicMs logged_mark_at = -1;
    // 本车的经济/发弹量/阵亡、能量机关、性能体系,变了才写一行:验收脚本靠它看
    // 兑换、复活、开能量机关、选性能体系有没有被服务器执行,复盘也看得到。
    struct SelfLog {
        QString supply, rune, performance;
    } self_log;
    QTimer freshness_timer;
    QObject::connect(&freshness_timer, &QTimer::timeout,
                     [&store, &reporter, &dashboard, &video, &reminders, &intake,
                      &recorder, &replay, &sample_links, &cfg, &recordings, speech,
                      &match_alerts, &alert_voice, assembly, dart, sentry, &controller,
                      &arm_rate, &seat_matched, &logged_event_at, &logged_sentry_result_at,
                      &logged_dart, &logged_mark_at, self_commands, revive_popup, &keyboard_mouse,
                      &self_log]() {
        // One clock read per tick, shared by the snapshot and the mode machine:
        // two reads would hand the machine an instant the snapshot never saw.
        const rm_terminal::MonotonicMs now = rm_terminal::monotonic_now();
        const auto snapshot = store.snapshot(now);
        reporter.inspect(snapshot);
        const auto events = snapshot.events.recent(snapshot.events.size());
        for (auto it = events.rbegin(); it != events.rend(); ++it) {
            if (it->received_at <= logged_event_at) continue;
            rm_terminal::StructuredLog::write(
                rm_terminal::LogLevel::info, QStringLiteral("match_event"),
                {QStringLiteral("id=%1").arg(it->event_id), QStringLiteral("level=%1").arg(it->level),
                 QStringLiteral("text=%1").arg(QString::fromStdString(it->text))});
        }
        if (!events.empty()) logged_event_at = std::max(logged_event_at, events.front().received_at);
        if (snapshot.sentry_result && snapshot.sentry_result->received_at > logged_sentry_result_at) {
            logged_sentry_result_at = snapshot.sentry_result->received_at;
            rm_terminal::StructuredLog::write(
                rm_terminal::LogLevel::info, QStringLiteral("sentry_result"),
                {QStringLiteral("command=%1").arg(snapshot.sentry_result->command_id),
                 QStringLiteral("code=%1").arg(snapshot.sentry_result->result_code)});
        }
        for (const auto& mark : snapshot.map_marks) {
            if (mark.received_at <= logged_mark_at) continue;
            logged_mark_at = mark.received_at;
            rm_terminal::StructuredLog::write(
                rm_terminal::LogLevel::info, QStringLiteral("map_mark_received"),
                {QStringLiteral("kind=%1").arg(mark.mode), QStringLiteral("target=%1").arg(mark.type),
                 QStringLiteral("enemy=%1").arg(mark.enemy_id),
                 QStringLiteral("x=%1").arg(mark.x, 0, 'f', 2), QStringLiteral("y=%1").arg(mark.y, 0, 'f', 2)});
        }
        if (snapshot.dart.target_id.value && snapshot.dart.gate.value) {
            const std::pair<std::uint32_t, std::uint32_t> dart_now{*snapshot.dart.target_id.value,
                                                                   *snapshot.dart.gate.value};
            if (logged_dart != dart_now)
                rm_terminal::StructuredLog::write(
                    rm_terminal::LogLevel::info, QStringLiteral("dart_status"),
                    {QStringLiteral("target=%1").arg(dart_now.first),
                     QStringLiteral("gate=%1").arg(dart_now.second)});
            logged_dart = dart_now;
        }
        for (const auto& alert : match_alerts.observe(snapshot, now)) {
            rm_terminal::StructuredLog::write(
                rm_terminal::LogLevel::info, QStringLiteral("alert"),
                {QStringLiteral("kind=%1").arg(rm_terminal::alert_kind_name(alert.kind)),
                 QStringLiteral("level=%1").arg(rm_terminal::alert_level_name(alert.level)),
                 QStringLiteral("text=%1").arg(alert.banner)});
            dashboard.showAlert(alert.banner, alert.level, now);
            if (alert.kind == rm_terminal::AlertKind::BaseAttacked) dashboard.flashBase(now);
            if (!replay) reminders.announce(alert_voice.pathFor(alert.speech));
        }
        dashboard.update(snapshot, &video, now);
        dashboard.setRecordingStatus(replay ? replay->statusText() : recorder.statusText());
        const bool link_up = !replay && intake.connected();
        dashboard.setLinkUp(link_up);
        if (assembly) {
            const auto arm = controller.stats();
            const double fps = arm_rate.update(arm.frames, now);
            assembly->setView(rm_terminal::assembly_view(snapshot));
            assembly->setLinkUp(link_up);
            assembly->setArm(arm.open ? std::optional<double>(fps) : std::nullopt,
                             arm.frames > 0 ? std::optional<std::array<float, 6>>(arm.joints)
                                            : std::nullopt);
            assembly->tick(now);
        }
        if (dart) {
            dart->setView(rm_terminal::dart_view(snapshot));
            dart->setLinkUp(link_up);
            dart->tick(now);
        }
        if (sentry) {
            sentry->setView(rm_terminal::sentry_view(snapshot));
            sentry->setLinkUp(link_up);
            sentry->tick(now);
        }
        if (self_commands) {
            const rm_terminal::SelfCommandView view = rm_terminal::self_command_view(snapshot);
            self_commands->setView(view);
            self_commands->setLinkUp(link_up);
            self_commands->tick(now);
            const auto text = [](const auto& value) {
                return value ? QString::number(*value) : QStringLiteral("--");
            };
            const auto log_change = [](QString& last, const QString& now_text, const char* event) {
                if (now_text == last) return;
                last = now_text;
                rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QString::fromLatin1(event),
                                                  {now_text});
            };
            const auto flag = [](const std::optional<bool>& value) {
                return value ? QString::number(*value ? 1 : 0) : QStringLiteral("--");
            };
            log_change(self_log.supply,
                       QStringLiteral("economy=%1 ammo=%2 dead=%3 free=%4")
                           .arg(text(view.economy), text(view.ammo), flag(view.dead),
                                flag(view.can_free_revive)),
                       "self_supply");
            log_change(self_log.rune,
                       QStringLiteral("status=%1 arms=%2").arg(text(view.rune_status), text(view.rune_arms)),
                       "rune_status");
            log_change(self_log.performance,
                       QStringLiteral("shooter=%1 chassis=%2").arg(text(view.shooter), text(view.chassis)),
                       "performance");
            if (revive_popup) {
                revive_popup->setLinkUp(link_up);
                revive_popup->setStatus(self_commands->reviveEchoText());
                revive_popup->setView(view);
            }
            if (keyboard_mouse) {
                keyboard_mouse->setLinkUp(link_up);
                // 阵亡就暂停键鼠控制、放开鼠标,复活框才点得到;复活后自动接着控制。
                keyboard_mouse->setSuspended(view.dead.value_or(false));
                QStringList held;
                const QString keys = rm_terminal::km_keys_text(keyboard_mouse->keysDown());
                if (!keys.isEmpty()) held << keys;
                if (keyboard_mouse->leftDown()) held << QStringLiteral("左键");
                if (keyboard_mouse->rightDown()) held << QStringLiteral("右键");
                self_commands->setKeyboardMouseState(keyboard_mouse->active() && !keyboard_mouse->suspended(),
                                                     held.join(QLatin1Char(' ')));
            }
        }
        if (!replay) {
            rm_terminal::SelfCheckInputs inputs;
            inputs.self_robot_id = cfg.self_robot_id;
            // 席位核对:服务器 RobotStaticStatus 报的编号和开机选的不一致,说明登录名填错。
            const auto self = snapshot.robots.find(
                rm_terminal::RobotId{static_cast<std::uint32_t>(cfg.self_robot_id)});
            if (self != snapshot.robots.end() &&
                self->second.statics.robot_id.quality == rm_terminal::Quality::Valid)
                inputs.server_robot_id = self->second.statics.robot_id.value;
            if (inputs.server_robot_id) {
                const bool matched = static_cast<int>(*inputs.server_robot_id) == cfg.self_robot_id;
                if (seat_matched != matched)
                    rm_terminal::StructuredLog::write(
                        matched ? rm_terminal::LogLevel::info : rm_terminal::LogLevel::error,
                        matched ? QStringLiteral("seat_confirmed") : QStringLiteral("seat_mismatch"),
                        {QStringLiteral("seat=%1").arg(cfg.self_robot_id),
                         QStringLiteral("server=%1").arg(*inputs.server_robot_id)});
                seat_matched = matched;
            }
            inputs.links = sample_links(now);
            inputs.recording = recorder.active();
            inputs.recording_error = recorder.error();
            const QStorageInfo disk(QDir(recordings).exists() ? recordings : QDir::homePath());
            if (disk.isValid()) inputs.free_bytes = disk.bytesAvailable();
            inputs.speech_available = speech->available();
            inputs.speech_hint = speech->unavailableReason();
            dashboard.setLinkReport(inputs.links);
            dashboard.setSelfCheck(rm_terminal::build_self_check(inputs));
        }
        // 同一个 now 和同一份快照喂给提醒:另取一次时钟会让调度看到一个快照
        // 从未见过的时刻,而两遍之间的间隔正是按这个时刻算的。
        reminders.observe(rm_terminal::reminder_inputs(snapshot, intake.connected()), now);
    });
    freshness_timer.start(250);

    // 第二遍的到点检查。250ms 的快照节拍对 1 秒的间隔来说太粗:间隔期满的时刻
    // 落在两次快照之间时,第二遍会被推迟到下一个快照。这个定时器只推进播放队列,
    // 不碰调度状态,所以跑得比快照密不会影响任何时序判定。
    QTimer reminder_timer;
    QObject::connect(&reminder_timer, &QTimer::timeout, [&reminders]() {
        reminders.tick(rm_terminal::monotonic_now());
    });
    reminder_timer.start(100);

    // Evidence capture renders the widget itself rather than grabbing the
    // screen: it needs no recording permission, captures nothing but this
    // window, and stays deterministic in the offscreen Qt platform plugin.
    int capture_status = rm_terminal::kExitSuccess;
    if (!screenshot_path.isEmpty() || !dump_layout_path.isEmpty()) {
        // Video needs longer than the widget itself: the decoder must spawn and
        // fill one frame, so capturing at 600ms would always show an empty pane.
        const int capture_delay_ms =
            qEnvironmentVariableIntValue("RM_TERMINAL_CAPTURE_DELAY_MS") > 0
                ? qEnvironmentVariableIntValue("RM_TERMINAL_CAPTURE_DELAY_MS")
                : 600;
        QTimer::singleShot(capture_delay_ms, &app, [&]() {
            if (!screenshot_path.isEmpty()) {
                const QPixmap frame = dashboard.grab();
                if (frame.isNull() || !frame.save(screenshot_path, "PNG")) {
                    qCritical().noquote() << "screenshot failed:" << screenshot_path;
                    capture_status = rm_terminal::kExitEvidenceFailure;
                } else {
                    qInfo().noquote() << "screenshot written:" << screenshot_path
                                      << frame.width() << "x" << frame.height();
                }
            }
            // Same callback as the screenshot, deliberately: a dump taken from a
            // separate timer could observe a different tick than the PNG, and the
            // two artifacts are meant to describe one instant.
            if (!dump_layout_path.isEmpty()) {
                const std::string dump = dashboard.layoutDump();
                QFile file(dump_layout_path);
                if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
                    file.write(dump.c_str(), qint64(dump.size())) != qint64(dump.size())) {
                    qCritical().noquote() << "layout dump failed:" << dump_layout_path;
                    capture_status = rm_terminal::kExitEvidenceFailure;
                } else {
                    file.close();
                    qInfo().noquote() << "layout dump written:" << dump_layout_path;
                }
            }
            app.quit();
        });
    }

    // The shutdown record must carry the code this process actually returns.
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     [&capture_status]() {
                         log_shutdown(QStringLiteral("terminal"), capture_status);
                     });
    const int result = app.exec();
    return capture_status != rm_terminal::kExitSuccess ? capture_status : result;
}
