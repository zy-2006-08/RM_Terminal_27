#include <QApplication>
#include <QCoreApplication>
#include <QScopeGuard>
#include <QThread>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QScreen>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QString>
#include <QStringList>
#include <QKeySequence>
#include <QShortcut>
#include <QProcess>
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
#include "input_method_guard.h"
#include "link_health.h"
#include "match_alerts.h"
#include "match_report.h"
#include "next_step.h"
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
#include "assembly_flow.h"
#include "dart_pane.h"
#include "hold_panels.h"
#include "km_control.h"
#include "purchase_popups.h"
#include "rune_key.h"
#include "revive_popup.h"
#include "self_command_pane.h"
#include "sentry_pane.h"
#include "settings_panel.h"
#include "terminal_settings.h"
#include "store.h"
#include "tactical_reminder_controller.h"
#include "ui_mode.h"
#include "ui_profile_store.h"
#include "video_receiver.h"
#include "video_relay.h"
#include "video_wall.h"

#if defined(RM_TERMINAL_HAS_SPEECH)
#include "macos_speech_backend.h"
#elif defined(RM_TERMINAL_PIPER_SPEECH)
#include "piper_speech_backend.h"
#endif

namespace
{

    // 从访达、程序坞双击 Mac 的 .app 时工作目录是 /:默认的 ./rm_terminal.conf 找不到,rm_terminal.log
    // 也写不进去,程序一打开就以退出码 4 退出。这种启动方式下把工作目录换到本机数据目录
    // (~/Library/Application Support/rm_terminal,录像也在这里),第一次打开时把包里带的配置放过去。
    // 在终端里运行时工作目录不是 /,什么都不变。
    void use_data_dir_when_launched_from_bundle()
    {
        if (QDir::currentPath() != QLatin1String("/"))
            return;
        const QDir contents(QCoreApplication::applicationDirPath() + QStringLiteral("/.."));
        if (!contents.exists(QStringLiteral("Info.plist")))
            return;
        const QString data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (!QDir().mkpath(data) || !QDir::setCurrent(data))
            return;
        const QString config = QString::fromLatin1(rm_terminal::kDefaultConfigFileName);
        const QString bundled = contents.filePath(QStringLiteral("Resources/") + config);
        if (!QFileInfo::exists(config) && QFile::copy(bundled, config))
            QFile::setPermissions(config, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup |
                                              QFileDevice::ReadOther);
    }

    // 席位从哪来:--seat > RM_SELF_ID > 配置里显式写的 self_robot_id;都没有时返回空,
    // 由界面弹选席位页(回放则用录像里记的席位)。
    bool prepare_runtime(const QString& config_path, rm_terminal::Config* cfg,
                         std::optional<int> cli_seat = std::nullopt, QString* seat_source = nullptr)
    {
        use_data_dir_when_launched_from_bundle();
        QString error;
        if (!rm_terminal::load_config(config_path, cfg, &error))
        {
            fprintf(stderr, "config error: %s\n", error.toUtf8().constData());
            return false;
        }
        QString source = cfg->self_robot_id_set ? QStringLiteral("config") : QString();
        const QString self_id = qEnvironmentVariable("RM_SELF_ID");
        if (!self_id.isEmpty())
        {
            bool ok = false;
            const int id = self_id.toInt(&ok);
            if (!ok || !rm_terminal::valid_self_robot_id(id))
            {
                fprintf(stderr,
                        "config error: invalid RM_SELF_ID=%s; expected 1, 3, 4, 6, "
                        "101, 103, 104, or 106\n",
                        self_id.toUtf8().constData());
                return false;
            }
            cfg->self_robot_id = id;
            source = QStringLiteral("env");
        }
        if (cli_seat)
        {
            cfg->self_robot_id = *cli_seat;
            source = QStringLiteral("cli");
        }
        if (seat_source)
            *seat_source = source;
        if (!rm_terminal::StructuredLog::open(cfg->log_destination, cfg->log_level, &error))
        {
            fprintf(stderr, "log error: %s\n", error.toUtf8().constData());
            return false;
        }
        rm_terminal::set_log_sink([](rm_terminal::LogLevel level, const QString& event,
                                     const QStringList& fields)
                                  { rm_terminal::StructuredLog::write(level, event, fields); });
        return true;
    }

    // Ctrl-C/kill 的默认动作是直接终止进程,析构函数不跑,录像最后一条会只写一半。
    // 信号处理函数里只能做异步信号安全的事,所以只记下信号号,由主线程定时器去 quit。
    // SA_RESETHAND:第二次 Ctrl-C 恢复默认动作,卡死时仍能强行结束。
    volatile std::sig_atomic_t g_quit_signal = 0;

    extern "C" void remember_quit_signal(int signal_number) { g_quit_signal = signal_number; }

    void install_quit_signals()
    {
        struct sigaction action{};
        action.sa_handler = remember_quit_signal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESETHAND;
        for (const int signal_number : {SIGINT, SIGTERM, SIGHUP})
            sigaction(signal_number, &action, nullptr);
    }

    void log_shutdown(const QString& mode, int code)
    {
        rm_terminal::StructuredLog::write(
            rm_terminal::LogLevel::info, QStringLiteral("shutdown"),
            {QStringLiteral("mode=%1").arg(mode), QStringLiteral("exit=%1").arg(code)});
        rm_terminal::set_log_sink(nullptr);
        rm_terminal::StructuredLog::close();
    }

    // 语音的平台边界收在这一个函数里,上层不再有 #if。
    rm_terminal::SpeechBackend* make_speech_backend(QObject* parent, const QString& speech_dir)
    {
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
                               const QString& seat_source = QString())
    {
        const auto seat = rm_terminal::Seat::fromRobotId(cfg.self_robot_id);
        return {QStringLiteral("mode=%1").arg(mode),
                QStringLiteral("seat=%1").arg(cfg.self_robot_id),
                QStringLiteral("role=%1").arg(seat ? rm_terminal::role_name(seat->role()) : QStringLiteral("none")),
                QStringLiteral("source=%1").arg(seat_source.isEmpty() ? QStringLiteral("default") : seat_source),
                QStringLiteral("platform=%1").arg(QString::fromLatin1(rm_terminal::platform_name())),
                QStringLiteral("mqtt_host=%1").arg(cfg.mqtt_host),
                QStringLiteral("mqtt_port=%1").arg(cfg.mqtt_port),
                QStringLiteral("udp_port=%1").arg(cfg.udp_port),
                QStringLiteral("video_hwaccel=%1").arg(cfg.video_hwaccel),
                QStringLiteral("self_robot_id=%1").arg(cfg.self_robot_id),
                QStringLiteral("stale_window_ms=%1").arg(cfg.stale_window_ms)};
    }

} // namespace

int main(int argc, char* argv[])
{
    // 录像、界面方案、Mac 上双击打开时的配置都放在按程序名找的目录里。Mac 的 .app 默认拿包名当程序名,
    // 不定死的话,包名「RM 终端」和直接运行的 rm_terminal 会用两个不同的目录。
    QCoreApplication::setApplicationName(QStringLiteral("rm_terminal"));
    QStringList args;
    for (int index = 1; index < argc; ++index)
        args.append(QString::fromLocal8Bit(argv[index]));

    QString config_path = qEnvironmentVariable(
        "RM_TERMINAL_CONFIG", QString::fromLatin1(rm_terminal::kDefaultConfigFileName));
    const int config_at = args.indexOf(QLatin1String("--config"));
    if (config_at >= 0)
    {
        if (config_at + 1 >= args.size())
        {
            qCritical().noquote() << "Missing path after --config";
            return rm_terminal::kExitBadArguments;
        }
        config_path = args.at(config_at + 1);
        args.removeAt(config_at + 1);
        args.removeAt(config_at);
    }
    if (args.contains(QLatin1String("--config")))
    {
        qCritical().noquote() << "Duplicate --config is not supported";
        return rm_terminal::kExitBadArguments;
    }

    QString screenshot_path;
    const int screenshot_at = args.indexOf(QLatin1String("--screenshot"));
    if (screenshot_at >= 0)
    {
        if (screenshot_at + 1 >= args.size())
        {
            qCritical().noquote() << "Missing path after --screenshot";
            return rm_terminal::kExitBadArguments;
        }
        screenshot_path = args.at(screenshot_at + 1);
        args.removeAt(screenshot_at + 1);
        args.removeAt(screenshot_at);
    }
    if (args.contains(QLatin1String("--screenshot")))
    {
        qCritical().noquote() << "Duplicate --screenshot is not supported";
        return rm_terminal::kExitBadArguments;
    }

    QString dump_layout_path;
    const int dump_layout_at = args.indexOf(QLatin1String("--dump-layout"));
    if (dump_layout_at >= 0)
    {
        if (dump_layout_at + 1 >= args.size())
        {
            qCritical().noquote() << "Missing path after --dump-layout";
            return rm_terminal::kExitBadArguments;
        }
        dump_layout_path = args.at(dump_layout_at + 1);
        args.removeAt(dump_layout_at + 1);
        args.removeAt(dump_layout_at);
    }
    if (args.contains(QLatin1String("--dump-layout")))
    {
        qCritical().noquote() << "Duplicate --dump-layout is not supported";
        return rm_terminal::kExitBadArguments;
    }

    std::optional<rm_terminal::UiMode> forced_mode;
    const int force_mode_at = args.indexOf(QLatin1String("--force-mode"));
    if (force_mode_at >= 0)
    {
        if (force_mode_at + 1 >= args.size())
        {
            qCritical().noquote() << "Missing mode after --force-mode; expected info or video";
            return rm_terminal::kExitBadArguments;
        }
        const QString requested = args.at(force_mode_at + 1);
        if (requested == QLatin1String("info"))
        {
            forced_mode = rm_terminal::UiMode::Info;
        }
        else if (requested == QLatin1String("video"))
        {
            forced_mode = rm_terminal::UiMode::Video;
        }
        else
        {
            qCritical().noquote() << "Invalid --force-mode value; expected info or video, got"
                                  << requested;
            return rm_terminal::kExitBadArguments;
        }
        args.removeAt(force_mode_at + 1);
        args.removeAt(force_mode_at);
    }
    if (args.contains(QLatin1String("--force-mode")))
    {
        qCritical().noquote() << "Duplicate --force-mode is not supported";
        return rm_terminal::kExitBadArguments;
    }

    std::optional<int> cli_seat;
    const int seat_at = args.indexOf(QLatin1String("--seat"));
    if (seat_at >= 0)
    {
        if (seat_at + 1 >= args.size())
        {
            qCritical().noquote()
                << "Missing seat after --seat; expected 1, 3, 4, 6, 101, 103, 104 or 106";
            return rm_terminal::kExitBadArguments;
        }
        bool ok = false;
        const int id = args.at(seat_at + 1).toInt(&ok);
        if (!ok || !rm_terminal::Seat::fromRobotId(id))
        {
            qCritical().noquote() << "Invalid --seat value; expected 1, 3, 4, 6, 101, 103, 104 or 106, got"
                                  << args.at(seat_at + 1);
            return rm_terminal::kExitBadArguments;
        }
        cli_seat = id;
        args.removeAt(seat_at + 1);
        args.removeAt(seat_at);
    }
    if (args.contains(QLatin1String("--seat")))
    {
        qCritical().noquote() << "Duplicate --seat is not supported";
        return rm_terminal::kExitBadArguments;
    }

    QString replay_path;
    const int replay_at = args.indexOf(QLatin1String("--replay"));
    if (replay_at >= 0)
    {
        if (replay_at + 1 >= args.size())
        {
            qCritical().noquote() << "Missing path after --replay";
            return rm_terminal::kExitBadArguments;
        }
        replay_path = args.at(replay_at + 1);
        args.removeAt(replay_at + 1);
        args.removeAt(replay_at);
    }
    if (args.contains(QLatin1String("--replay")))
    {
        qCritical().noquote() << "Duplicate --replay is not supported";
        return rm_terminal::kExitBadArguments;
    }

    // 录像片段(复盘报告里每件事前 10 秒到后 5 秒):--from/--to 只放这一段(秒,从开录算),
    // --export-video 把这一段离屏画成 MP4(要 ffmpeg),画完退出。都只能跟 --replay 一起用。
    std::optional<double> clip_from_s, clip_to_s;
    QString export_video;
    for (const char* flag : {"--from", "--to", "--export-video"})
    {
        const int at = args.indexOf(QLatin1String(flag));
        if (at < 0)
            continue;
        if (at + 1 >= args.size())
        {
            qCritical().noquote() << "Missing value after" << flag;
            return rm_terminal::kExitBadArguments;
        }
        const QString value = args.at(at + 1);
        args.removeAt(at + 1);
        args.removeAt(at);
        if (args.contains(QLatin1String(flag)))
        {
            qCritical().noquote() << "Duplicate" << flag << "is not supported";
            return rm_terminal::kExitBadArguments;
        }
        if (QLatin1String(flag) == QLatin1String("--export-video"))
        {
            export_video = value;
            continue;
        }
        bool ok = false;
        const double seconds = value.toDouble(&ok);
        if (!ok || seconds < 0)
        {
            qCritical().noquote() << "Invalid" << flag << "value; expected seconds, got" << value;
            return rm_terminal::kExitBadArguments;
        }
        (QLatin1String(flag) == QLatin1String("--from") ? clip_from_s : clip_to_s) = seconds;
    }
    if ((clip_from_s || clip_to_s || !export_video.isEmpty()) && replay_path.isEmpty())
    {
        qCritical().noquote() << "--from, --to and --export-video only work with --replay";
        return rm_terminal::kExitBadArguments;
    }
    if (!export_video.isEmpty() && !clip_to_s)
    {
        qCritical().noquote() << "--export-video needs --to";
        return rm_terminal::kExitBadArguments;
    }
    if (clip_from_s && clip_to_s && *clip_to_s <= *clip_from_s)
    {
        qCritical().noquote() << "--to must be after --from";
        return rm_terminal::kExitBadArguments;
    }

    // 测试会拉起真实程序,默认自动录制曾把验收产物写进用户录像目录。
    const bool no_record = args.removeOne(QLatin1String("--no-record"));

    const bool safe_smoke = args.size() == 1 && args.at(0) == QLatin1String("--safe-smoke");
    const bool diagnostic = args.size() == 4 && args.at(0) == QLatin1String("--diagnostic");

    // Rejected rather than ignored. Both of these modes are headless, so there is
    // no layout to force; and because the flag was stripped above, staying silent
    // would let an evidence-capture command appear to succeed while proving nothing.
    if (forced_mode.has_value() && (safe_smoke || diagnostic))
    {
        qCritical().noquote()
            << "--force-mode requires the GUI; it cannot combine with --safe-smoke or --diagnostic";
        return rm_terminal::kExitBadArguments;
    }
    if (!dump_layout_path.isEmpty() && (safe_smoke || diagnostic))
    {
        qCritical().noquote() << "--dump-layout requires the GUI; it cannot combine with"
                                 " --safe-smoke or --diagnostic";
        return rm_terminal::kExitBadArguments;
    }
    if (!replay_path.isEmpty() && (safe_smoke || diagnostic))
    {
        qCritical().noquote() << "--replay requires the GUI; it cannot combine with"
                                 " --safe-smoke or --diagnostic";
        return rm_terminal::kExitBadArguments;
    }

    if (safe_smoke)
    {
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

    if (diagnostic)
    {
        QCoreApplication app(argc, argv);
        bool port_ok = false;
        bool seconds_ok = false;
        const int port = args.at(2).toInt(&port_ok);
        const int seconds = args.at(3).toInt(&seconds_ok);
        if (!port_ok || !seconds_ok || port <= 0 || seconds <= 0 || seconds > 30)
        {
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
                         [](const QString& text)
                         { qInfo().noquote() << text; });
        if (!intake.start())
        {
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

    if (!args.isEmpty())
    {
        qCritical().noquote() << "Unknown argument; supported: --safe-smoke, --config <path>,"
                                 " --seat <id>, --screenshot <path>, --force-mode <info|video>,"
                                 " --replay <path> [--from/--to <s>, --export-video <mp4>],"
                                 " --dump-layout <path>, --no-record,"
                                 " --diagnostic <host> <port> <seconds>";
        return rm_terminal::kExitBadArguments;
    }

    QApplication app(argc, argv);
    // 中文输入法只给打字框:不然字母键(Y 复活、H/O/I 兑换、P 设置、WASD 键鼠控制)会被拿去拼字。
    auto* input_method_guard = new rm_terminal::InputMethodGuard(&app);
    app.installEventFilter(input_method_guard);
    rm_terminal::Config cfg;
    QString seat_source;
    if (!prepare_runtime(config_path, &cfg, cli_seat, &seat_source))
        return rm_terminal::kExitConfigError;
    QObject::connect(input_method_guard, &rm_terminal::InputMethodGuard::blocked, &app, [](const QString& what)
                     { rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("input_method_blocked"),
                                                         {QStringLiteral("object=%1").arg(what)}); });
    // 席位在连网和建主界面之前定下来:登录名(MQTT clientID)和布局都取决于它,
    // 本次运行内不能再改。回放用录像里记的席位,不弹选席位页。
    // 界面设置按操作手存。选席位页顺便选人;跳过选席位页(--seat、RM_SELF_ID、回放)时
    // 用这台电脑上次那个人,进去后在「界面设置」里换。
    const rm_terminal::UiProfileRepository ui_profile_files(rm_terminal::UiProfileRepository::defaultDirectory());
    QString picked_operator;
    if (seat_source.isEmpty() && replay_path.isEmpty())
    {
        rm_terminal::SeatPicker picker;
        rm_terminal::UiProfileLoad known = ui_profile_files.load();
        rm_terminal::normalize(&known.book);
        QStringList names;
        for (const auto& profile : known.book.operators)
            names << profile.name;
        picker.setOperators(names, known.book.last_operator);
        const auto seat = picker.exec();
        picked_operator = picker.operatorName();
        if (!seat)
        {
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
    QObject::connect(&quit_signal_poll, &QTimer::timeout, &app, [&app]()
                     {
        if (g_quit_signal == 0) return;
        rm_terminal::StructuredLog::write(
            rm_terminal::LogLevel::info, QStringLiteral("quit_signal"),
            {QStringLiteral("signal=%1").arg(static_cast<int>(g_quit_signal))});
        g_quit_signal = 0;
        app.quit(); });
    quit_signal_poll.start(100);

    rm_terminal::Store store(cfg.stale_window_ms);
    rm_terminal::MqttIntake intake(store, cfg.mqtt_host, cfg.mqtt_port,
                                   cfg.self_robot_id);
    QObject::connect(&intake, &rm_terminal::MqttIntake::diagnostic,
                     [](const QString& text)
                     { qInfo().noquote() << text; });

    rm_terminal::Recorder recorder;
    intake.setMessageObserver([&recorder](const QString& topic, const QByteArray& payload)
                              { recorder.recordMqtt(topic, payload); });

    const QString recordings =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("recordings"));
    if (replay_path.isEmpty() && !no_record)
    {
        rm_terminal::RecordingMetadata metadata;
        metadata.self_robot_id = cfg.self_robot_id;
        metadata.mqtt_host = cfg.mqtt_host;
        metadata.mqtt_port = cfg.mqtt_port;
        metadata.udp_port = cfg.udp_port;
        QString record_error;
        if (!recorder.start(recordings, metadata, &record_error))
        {
            qWarning().noquote() << "recording unavailable:" << record_error;
        }
        else
        {
            qInfo().noquote() << "recording:" << recorder.path();
        }
    }

    intake.setKeepAlive(cfg.mqtt_keepalive_s);
    if (replay_path.isEmpty() && !intake.start())
    {
        rm_terminal::StructuredLog::write(
            rm_terminal::LogLevel::warning, QStringLiteral("reconnect_mqtt"),
            {QStringLiteral("state=unavailable"),
             QStringLiteral("host=%1").arg(cfg.mqtt_host),
             QStringLiteral("port=%1").arg(cfg.mqtt_port)});
    }

    rm_terminal::VideoReceiver video;
    std::unique_ptr<rm_terminal::VideoRelay> relay;
    // 实况的本机图传在自己的线程里收包和解码(为什么见 video_receiver.h)。退出时先在那条线程里
    // 停下接收器、把它挪回主线程,再停线程:接收器析构时和它的套接字、解码进程同在主线程。
    QThread video_thread;
    video_thread.setObjectName(QStringLiteral("video"));
    const auto stop_video_thread = qScopeGuard([&video, &video_thread]
                                               {
        if (!video_thread.isRunning()) return;
        QThread* const home = QThread::currentThread();
        QMetaObject::invokeMethod(
            &video, [&video, home] { video.stop(); video.moveToThread(home); }, Qt::BlockingQueuedConnection);
        video_thread.quit();
        video_thread.wait(); });
    const bool commander_seat = rm_terminal::Seat::fromRobotId(cfg.self_robot_id) &&
                                rm_terminal::Seat::fromRobotId(cfg.self_robot_id)->role() == rm_terminal::Role::Commander;
    const bool relay_video = replay_path.isEmpty() && !commander_seat && !cfg.video_relay_host.isEmpty();
    // 这个回调在图传线程里跑:Recorder 自带锁,直接写;转发用的 UDP 口在主线程,排回主线程发,顺序不变。
    video.setDatagramObserver([&recorder, &relay, relay_video](const QByteArray& datagram)
                              {
        recorder.recordVideo(datagram);
        if (relay_video)
            QMetaObject::invokeMethod(qApp, [&relay, datagram] {
                if (relay) relay->forward(datagram);
            }); });

    std::unique_ptr<rm_terminal::ReplayPlayer> replay;
    if (!replay_path.isEmpty())
    {
        if (!video.startInjected())
        {
            rm_terminal::StructuredLog::write(
                rm_terminal::LogLevel::warning, QStringLiteral("reconnect_udp"),
                {QStringLiteral("state=replay_decoder_failed")});
        }
        replay = std::make_unique<rm_terminal::ReplayPlayer>(store, video);
        QString replay_error;
        if (!replay->start(replay_path, &replay_error))
        {
            qCritical().noquote() << "replay failed:" << replay_error;
            rm_terminal::StructuredLog::write(
                rm_terminal::LogLevel::error, QStringLiteral("replay"),
                {QStringLiteral("state=open_failed"),
                 QStringLiteral("path=%1").arg(replay_path)});
            log_shutdown(QStringLiteral("replay"), rm_terminal::kExitStartFailure);
            return rm_terminal::kExitStartFailure;
        }
        qInfo().noquote() << "replaying:" << replay_path;
        if (clip_from_s)
            replay->seek(static_cast<qint64>(*clip_from_s * 1000));
        if (seat_source.isEmpty() && rm_terminal::Seat::fromRobotId(replay->selfRobotId()))
        {
            cfg.self_robot_id = replay->selfRobotId();
            seat_source = QStringLiteral("recording");
        }
        rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("startup"),
                                          startup_fields(QStringLiteral("replay"), cfg, seat_source));
    }
    else
    {
        rm_terminal::VideoEndpoint endpoint;
        // 真实选手端按协议把图传发到本机地址（192.168.12.2:3334），只绑回环就收不到。
        endpoint.address = QHostAddress::AnyIPv4;
        endpoint.port = static_cast<quint16>(cfg.udp_port);
        endpoint.hwaccel = cfg.video_hwaccel;
        if (!commander_seat)
        {
            video.moveToThread(&video_thread);
            video_thread.start();
        }
        if (!commander_seat && !video.start(endpoint))
        {
            rm_terminal::StructuredLog::write(
                rm_terminal::LogLevel::warning, QStringLiteral("reconnect_udp"),
                {QStringLiteral("state=start_failed"),
                 QStringLiteral("port=%1").arg(cfg.udp_port)});
        }
    }

    // 指挥手没有自己的图传(本机那一路不开),看的是队友终端转发过来的:规则 3.3.1 写明操作间里的自定义终端
    // 可以有线互连通信。重装和两台步兵各占一个口,地面席位把自己收到的图传原样转发过去。回放时都不开:
    // 录像里没有别人的图传,看录像也不该往外发。
    std::vector<std::pair<std::uint32_t, std::unique_ptr<rm_terminal::VideoReceiver>>> wall;
    if (!replay && commander_seat)
    {
        for (const std::uint32_t robot : rm_terminal::wall_robots(static_cast<std::uint32_t>(cfg.self_robot_id)))
        {
            auto receiver = std::make_unique<rm_terminal::VideoReceiver>();
            rm_terminal::VideoEndpoint endpoint;
            endpoint.address = QHostAddress::AnyIPv4;
            endpoint.port = rm_terminal::wall_port(cfg.video_wall_port, static_cast<int>(robot));
            const bool listening = receiver->start(endpoint);
            rm_terminal::StructuredLog::write(
                listening ? rm_terminal::LogLevel::info : rm_terminal::LogLevel::warning, QStringLiteral("video_wall"),
                {QStringLiteral("state=%1").arg(listening ? QStringLiteral("listening") : QStringLiteral("start_failed")),
                 QStringLiteral("robot=%1").arg(robot), QStringLiteral("port=%1").arg(endpoint.port)});
            wall.emplace_back(robot, std::move(receiver));
        }
    }
    if (!replay && !commander_seat && !cfg.video_relay_host.isEmpty())
        relay = std::make_unique<rm_terminal::VideoRelay>(QHostAddress(cfg.video_relay_host),
                                                          rm_terminal::wall_port(cfg.video_wall_port, cfg.self_robot_id));

    // 机械臂 -> 串口 -> 这里 -> MQTT CustomControl -> 车。回放时不开串口:看录像不该
    // 让桌上的机械臂数据被发出去。每帧只在收到时转发一次,断线或发不出去就丢掉,
    // 绝不补发 —— 补发的是旧姿态,车上的机械臂会去追一个早已过去的位置。
    rm_terminal::ControllerLink controller(replay ? QString() : cfg.controller_serial,
                                           cfg.controller_baud);
    std::optional<rm_terminal::MonotonicMs> last_forward_at;
    QObject::connect(&controller, &rm_terminal::ControllerLink::frameReceived, &app,
                     [&intake, &recorder, &last_forward_at](const QByteArray& data)
                     {
                         // 2026 协议:CustomControl 最高 75Hz。固件发 25Hz,这里只防
                         // 接错设备时把服务器打爆。
                         constexpr rm_terminal::MonotonicMs kMinIntervalMs = 14;
                         const rm_terminal::MonotonicMs now = rm_terminal::monotonic_now();
                         if (last_forward_at && now - *last_forward_at < kMinIntervalMs)
                             return;
                         rm::CustomControl message;
                         message.set_data(data.constData(), static_cast<std::size_t>(data.size()));
                         const std::string bytes = message.SerializeAsString();
                         if (!intake.publish("CustomControl", bytes))
                             return;
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
                      &feedback_invalid](const QByteArray& data)
                     {
                         // 0x0310 最高 50Hz,这里只防车端程序写错时把串口打满。
                         constexpr rm_terminal::MonotonicMs kMinIntervalMs = 15;
                         const rm_terminal::MonotonicMs now = rm_terminal::monotonic_now();
                         if (last_feedback_at && now - *last_feedback_at < kMinIntervalMs)
                             return;
                         const auto frame = rm_terminal::arm_feedback_frame(data, feedback_seq);
                         if (!frame)
                         {
                             if (!feedback_invalid)
                                 rm_terminal::StructuredLog::write(
                                     rm_terminal::LogLevel::warning, QStringLiteral("arm_feedback"),
                                     {QStringLiteral("state=invalid"),
                                      QStringLiteral("bytes=%1").arg(data.size())});
                             feedback_invalid = true;
                             return;
                         }
                         feedback_invalid = false;
                         if (!controller.send(*frame))
                             return;
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
    if (const auto seat = rm_terminal::Seat::fromRobotId(cfg.self_robot_id))
        alert_settings.role = seat->role();
    alert_settings.control_gap = cfg.alert_control_gap;
    alert_settings.disabled = cfg.alerts_disabled;
    rm_terminal::MatchAlerts match_alerts(alert_settings);
    rm_terminal::AlertVoice alert_voice(make_speech_backend(&app, cfg.speech_dir));
    // 步兵进入 15 秒自检阶段还没选性能体系时念的那一句。
    const QString performance_phrase = QStringLiteral("还没选性能体系");
    if (!replay)
        alert_voice.prepare(cache_dir,
                            rm_terminal::match_alert_phrases() + rm_terminal::next_step_phrases() +
                                QStringList{performance_phrase},
                            reminders.currentVoice(),
                            reminders.config().rate_wpm);

    rm_terminal::Dashboard dashboard(cfg);
    // 这一局每秒一笔的流水:指挥手的大局画趋势线,每局结束当场写复盘网页(大局判断表.md)。
    rm_terminal::MatchLog match_log;
    dashboard.setMatchLog(&match_log);
    const QString reports =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("reports"));
    dashboard.attachReminders(&reminders);
    const rm_terminal::Role seat_role = rm_terminal::Seat::fromRobotId(cfg.self_robot_id)
                                            ? rm_terminal::Seat::fromRobotId(cfg.self_robot_id)->role()
                                            : rm_terminal::Role::Infantry;
    if (replay && seat_role == rm_terminal::Role::Commander)
    {
        std::vector<rm_terminal::WallFeed> feeds;
        for (const std::uint32_t robot : rm_terminal::wall_robots(static_cast<std::uint32_t>(cfg.self_robot_id)))
            feeds.push_back(rm_terminal::wall_feed(robot, nullptr));
        dashboard.setWallFeeds(std::move(feeds));
    }
    rm_terminal::UiProfileController ui_profiles(ui_profile_files, seat_role);
    if (!picked_operator.isEmpty())
        ui_profiles.switchOperator(picked_operator);
    // 操作手关掉的提醒叠加在配置文件 alerts_disabled 之上,语音开关只管事件提醒的声音。
    bool alert_voice_on = true;
    const auto apply_alert_choices = [&]
    {
        const rm_terminal::UiOptions& options = ui_profiles.activeScheme().options;
        QStringList disabled = cfg.alerts_disabled;
        for (const QString& kind : options.alerts_muted)
            if (!disabled.contains(kind))
                disabled << kind;
        match_alerts.setDisabled(disabled);
        alert_voice_on = options.alert_voice;
    };
    apply_alert_choices();
    QObject::connect(&ui_profiles, &rm_terminal::UiProfileController::changed, &dashboard, apply_alert_choices);
    dashboard.attachUiProfiles(&ui_profiles, cfg.alerts_disabled);
    QObject::connect(&reminders, &rm_terminal::ReminderController::reminderDue, &dashboard,
                     [&dashboard](const QString& text)
                     {
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
    if (const QString wanted = qEnvironmentVariable("RM_TERMINAL_CAPTURE_SIZE"); !wanted.isEmpty())
    {
        const QStringList parts = wanted.split(QLatin1Char('x'));
        bool w_ok = false;
        bool h_ok = false;
        const int w = parts.size() == 2 ? parts[0].toInt(&w_ok) : 0;
        const int h = parts.size() == 2 ? parts[1].toInt(&h_ok) : 0;
        if (w_ok && h_ok && w >= 1280 && h >= 760)
            window_size = QSize(w, h);
        else
            qWarning().noquote() << "ignoring RM_TERMINAL_CAPTURE_SIZE=" << wanted;
    }
    dashboard.resize(window_size);
    // 屏幕清单写进日志:display_screen 要填这里的名字。
    QStringList screen_names;
    for (const QScreen* screen : QGuiApplication::screens())
        screen_names << QStringLiteral("%1:%2x%3").arg(screen->name()).arg(screen->geometry().width()).arg(screen->geometry().height());
    rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("screens"),
                                      {QStringLiteral("names=%1").arg(screen_names.join(QLatin1Char(',')))});
    bool fullscreen = false;
    // 有些屏幕的可用高度装不下 900 的窗口，底部的回放控制条会被挤出屏幕。
    // 实跑时按可用区域收一点，离屏截图仍保持固定尺寸。
    if (QGuiApplication::platformName() != QLatin1String("offscreen"))
    {
        const QScreen* target = QGuiApplication::primaryScreen();
        if (cfg.display_fullscreen && !cfg.display_screen.isEmpty())
        {
            target = nullptr;
            for (QScreen* screen : QGuiApplication::screens())
                if (screen->name() == cfg.display_screen)
                    target = screen;
            if (!target)
            {
                rm_terminal::StructuredLog::write(
                    rm_terminal::LogLevel::warning, QStringLiteral("display"),
                    {QStringLiteral("state=screen_missing"), QStringLiteral("wanted=%1").arg(cfg.display_screen)});
                target = QGuiApplication::primaryScreen();
            }
        }
        if (target && cfg.display_fullscreen)
        {
            dashboard.setGeometry(target->geometry());
            fullscreen = true;
        }
        else if (target)
        {
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
    if (forced_mode.has_value())
        dashboard.forceMode(forced_mode);
    const rm_terminal::MonotonicMs startup_now = rm_terminal::monotonic_now();
    dashboard.update(store.snapshot(startup_now), &video, startup_now);
    dashboard.setRecordingStatus(replay ? replay->statusText() : recorder.statusText());
    if (replay)
    {
        dashboard.showReplayControls(true);
        dashboard.setReplayPaused(replay->paused());
        dashboard.setReplayProgress(replay->positionMs(), replay->durationMs());
        dashboard.setReplaySeekHandler([&replay](qint64 position_ms)
                                       { replay->seek(position_ms); });
        dashboard.setReplayPauseHandler([&replay]
                                        { replay->togglePause(); });
        dashboard.setReplaySpeedHandler([&replay](double speed)
                                        { replay->setSpeed(speed); });
        QObject::connect(replay.get(), &rm_terminal::ReplayPlayer::progress, &dashboard,
                         &rm_terminal::Dashboard::setReplayProgress);
        QObject::connect(replay.get(), &rm_terminal::ReplayPlayer::pausedChanged, &dashboard,
                         &rm_terminal::Dashboard::setReplayPaused);
    }
    // 角色面板放进这个席位的卡片里,不管启动时的方案里有没有这块。回放时不发任何指令:intake 没启动,
    // publish 必然返回假。
    const auto send = [&intake, &replay](const char* topic, const std::string& payload)
    {
        return !replay && intake.publish(topic, payload);
    };
    dashboard.setCommandSender(send);
    // 重装装配不再占一块面板:装配是拿真的自定义控制器做的,终端只发开始、确认、退出。照官方选手端,
    // 重装按 H 打开的框里选难度,装配中屏幕下方正中显示进度(assembly_flow.h)。
    rm_terminal::AssemblyFlow* assembly = nullptr;
    rm_terminal::DartPane* dart = nullptr;
    rm_terminal::SentryPane* sentry = nullptr;
    if (QWidget* card = dashboard.roleCard(rm_terminal::PaneId::Dart))
    {
        dart = new rm_terminal::DartPane(card);
        dashboard.placePane(rm_terminal::PaneId::Dart, dart);
        dart->setSender(send);
        // 飞镖键位照官方选手端。只有指挥手有飞镖面板,指挥手又没有兑换/复活/装配的 Y、N、L,不会撞键。
        const auto dart_key = [&dashboard](int key, auto action)
        {
            auto* shortcut = new QShortcut(QKeySequence(key), &dashboard);
            shortcut->setContext(Qt::ApplicationShortcut);
            // 按住 L 不连发:按一下只发一枚。
            shortcut->setAutoRepeat(false);
            QObject::connect(shortcut, &QShortcut::activated, &dashboard, action);
        };
        dart_key(Qt::Key_J, [dart]
                 { dart->cycleTarget(); });
        dart_key(Qt::Key_F, [dart]
                 { dart->requestOpen(); });
        dart_key(Qt::Key_Y, [dart]
                 { dart->confirmOpen(); });
        dart_key(Qt::Key_N, [dart]
                 { dart->cancelOpen(); });
        dart_key(Qt::Key_L, [dart]
                 { dart->launch(); });
    }
    if (QWidget* card = dashboard.roleCard(rm_terminal::PaneId::Sentry))
    {
        sentry = new rm_terminal::SentryPane(card);
        dashboard.placePane(rm_terminal::PaneId::Sentry, sentry);
        sentry->setSender(send);
    }
    // 「本车」面板删掉了,画面上不再有它。这个对象留着、不显示:兑换、复活、开能量机关、选性能体系
    // 的发送、限频和回显都在它里面,按键(H、O/I、Y/F1、F、P)和弹框都走它。步兵和重装才有。
    rm_terminal::SelfCommandPane* self_commands = nullptr;
    if (seat_role != rm_terminal::Role::Commander)
    {
        self_commands = new rm_terminal::SelfCommandPane(&dashboard);
        self_commands->hide();
        self_commands->setSender(send);
    }
    // P 设置面板照官方选手端:按 P 打开 / 关闭,所有席位都有。终端自己的几项按这台电脑存。
    auto* settings_panel = new rm_terminal::SettingsPanel(&dashboard);
    const QString settings_path = rm_terminal::terminal_settings_path();
    QString settings_error;
    rm_terminal::TerminalSettings terminal_settings = rm_terminal::load_terminal_settings(settings_path, &settings_error);
    if (!settings_error.isEmpty())
        rm_terminal::StructuredLog::write(rm_terminal::LogLevel::warning, QStringLiteral("terminal_settings"),
                                          {QStringLiteral("state=invalid"), QStringLiteral("error=%1").arg(settings_error)});
    settings_panel->setSettings(terminal_settings);
    // 复活框照官方选手端:阵亡才弹出;读条结束后按 Y(或点「免费复活」)确认,点「金币复活」
    // 花钱。活着时按 Y 什么都不发。
    rm_terminal::RevivePopup* revive_popup = nullptr;
    // 兑换照官方选手端:H 远程兑换框(Ctrl+1 回血、Ctrl+2 17mm、Ctrl+3 42mm),O/I 补给区补弹面板
    // (17mm / 42mm),都要按 Y 二次确认、N 取消。本车面板上的按钮也走同一个确认框。
    rm_terminal::PurchaseFlow* purchases = nullptr;
    if (self_commands)
    {
        purchases = new rm_terminal::PurchaseFlow(&dashboard);
        purchases->setSender(
            [self_commands](const rm_terminal::PurchaseOffer& offer)
            { return self_commands->sendPurchase(offer); });
        self_commands->setPurchaseRequest([purchases](rm_terminal::PurchaseKind kind, std::uint32_t rounds)
                                          { purchases->ask(kind, rounds); });
        if (seat_role == rm_terminal::Role::Hero)
        {
            assembly = new rm_terminal::AssemblyFlow(&dashboard);
            assembly->setSender(send);
            assembly->setNeighbor(purchases->remotePopup());
            // Enter 开始;确认框开着时 Enter 不算,免得想确认回血却开始了装配。开始了就把框关掉。
            const auto start_assembly = [assembly, purchases]
            {
                if (!purchases->confirmOpen() && assembly->start())
                    purchases->toggleRemote();
            };
            purchases->setAssemblyActions([assembly](std::uint32_t level)
                                          { assembly->choose(level); }, start_assembly);
            QObject::connect(assembly, &rm_terminal::AssemblyFlow::rowChanged, purchases,
                             [assembly, purchases]
                             { purchases->setAssemblyRow(assembly->row()); });
            purchases->setAssemblyRow(assembly->row());
            QObject::connect(assembly, &rm_terminal::AssemblyFlow::commandSent, &dashboard,
                             [](std::uint32_t operation, std::uint32_t difficulty, bool ok)
                             {
                                 rm_terminal::StructuredLog::write(
                                     rm_terminal::LogLevel::info, QStringLiteral("assembly_command"),
                                     {QStringLiteral("operation=%1").arg(operation),
                                      QStringLiteral("difficulty=%1").arg(difficulty),
                                      QStringLiteral("state=%1").arg(ok ? "sent" : "not_sent")});
                             });
        }
        const auto shortcut = [&dashboard](const QKeySequence& keys, auto action)
        {
            auto* key = new QShortcut(keys, &dashboard);
            key->setContext(Qt::ApplicationShortcut);
            QObject::connect(key, &QShortcut::activated, &dashboard, action);
        };
        // macOS 上 Qt::CTRL 是 Command 键,物理 Ctrl 是 META(和 km_control.cpp 一样)。
#if defined(Q_OS_MACOS)
        const int ctrl = Qt::META;
#else
        const int ctrl = Qt::CTRL;
#endif
        // 重装按 H 打开的框里还有装配难度;装配中按 H 不开框,免得 Y 按成买东西。
        shortcut(QKeySequence(Qt::Key_H), [purchases, settings_panel, assembly]
                 {
            if (assembly && assembly->canExit()) {
                assembly->busy();
                return;
            }
            settings_panel->setOpen(false);
            purchases->toggleRemote();
            if (assembly && purchases->remoteOpen()) assembly->boxOpened(); });
        shortcut(QKeySequence(Qt::Key_O), [purchases, settings_panel]
                 {
            settings_panel->setOpen(false);
            purchases->toggleSupply(rm_terminal::Caliber::Small); });
        shortcut(QKeySequence(Qt::Key_I), [purchases, settings_panel]
                 {
            settings_panel->setOpen(false);
            purchases->toggleSupply(rm_terminal::Caliber::Large); });
        shortcut(QKeySequence(ctrl | Qt::Key_1), [purchases]
                 { purchases->chooseRemote(0); });
        shortcut(QKeySequence(ctrl | Qt::Key_2), [purchases]
                 { purchases->chooseRemote(1); });
        shortcut(QKeySequence(ctrl | Qt::Key_3), [purchases]
                 { purchases->chooseRemote(2); });
        shortcut(QKeySequence(Qt::Key_N), [purchases]
                 { purchases->no(); });

        revive_popup = new rm_terminal::RevivePopup(&dashboard);
        dashboard.setEliminatedOverlayReplaced(true);
        revive_popup->setActions([self_commands]
                                 { self_commands->confirmRevive(); },
                                 [self_commands]
                                 { self_commands->payRevive(); });
        // Y:兑换确认框开着时是「确定」;重装装配中是「确认装配」;都不是才是免费复活。
        shortcut(QKeySequence(Qt::Key_Y), [self_commands, purchases, assembly]
                 {
            if (purchases->yes()) return;
            if (assembly && assembly->confirm()) return;
            self_commands->confirmRevive(); });
        if (assembly)
        {
            // 1–4 选难度、Enter 开始只在 H 框开着时归终端;L 退出装配只在装配中归终端。平时不拦这几个键,
            // 给方案起名字时照样能打数字和 L。
            std::vector<QShortcut*> box_keys;
            const auto box_key = [&dashboard, &box_keys](int key, auto action)
            {
                auto* shortcut_key = new QShortcut(QKeySequence(key), &dashboard);
                shortcut_key->setContext(Qt::ApplicationShortcut);
                shortcut_key->setEnabled(false);
                QObject::connect(shortcut_key, &QShortcut::activated, &dashboard, action);
                box_keys.push_back(shortcut_key);
                return shortcut_key;
            };
            box_key(Qt::Key_1, [assembly]
                    { assembly->choose(1); });
            box_key(Qt::Key_2, [assembly]
                    { assembly->choose(2); });
            box_key(Qt::Key_3, [assembly]
                    { assembly->choose(3); });
            box_key(Qt::Key_4, [assembly]
                    { assembly->choose(4); });
            const auto start_assembly = [assembly, purchases]
            {
                if (!purchases->confirmOpen() && assembly->start())
                    purchases->toggleRemote();
            };
            box_key(Qt::Key_Return, start_assembly);
            box_key(Qt::Key_Enter, start_assembly);
            QObject::connect(purchases, &rm_terminal::PurchaseFlow::remoteOpenChanged, &dashboard,
                             [box_keys](bool open)
                             {
                                 for (QShortcut* key : box_keys)
                                     key->setEnabled(open);
                             });
            auto* exit_key = new QShortcut(QKeySequence(Qt::Key_L), &dashboard);
            exit_key->setContext(Qt::ApplicationShortcut);
            exit_key->setEnabled(false);
            QObject::connect(exit_key, &QShortcut::activated, &dashboard, [assembly]
                             { assembly->exit(); });
            QObject::connect(assembly, &rm_terminal::AssemblyFlow::canExitChanged, exit_key, &QShortcut::setEnabled);
        }
        // F1:花金币立即复活,照官方选手端(手册第 9 页)。活着时什么都不发。
        shortcut(QKeySequence(Qt::Key_F1), [self_commands]
                 { self_commands->payRevive(); });
    }
    // 键鼠控制只在实况下有:回放没有链路,录像里也没有这台电脑的键盘。
    std::unique_ptr<rm_terminal::KmCapture> keyboard_mouse;
    // 键鼠控制暂停的原因:阵亡(点复活框)或开着补弹面板(点数量)。状态条和日志用。
    QString km_pause_reason;
    if (self_commands && !replay)
    {
        keyboard_mouse = std::make_unique<rm_terminal::KmCapture>(&dashboard);
        rm_terminal::KmCapture* km = keyboard_mouse.get();
        km->setSender(send);
        std::vector<int> passthrough{Qt::Key_H, Qt::Key_O, Qt::Key_I, Qt::Key_P, Qt::Key_Y,
                                     Qt::Key_N, Qt::Key_1, Qt::Key_2, Qt::Key_3, Qt::Key_F1,
                                     Qt::Key_Tab, Qt::Key_Backtab, Qt::Key_QuoteLeft, Qt::Key_AsciiTilde};
        // 重装的装配键:框里 4 选四级、Enter 开始,装配中 L 退出。这几个键本来就不发给车。
        if (assembly)
            passthrough.insert(passthrough.end(), {Qt::Key_4, Qt::Key_L, Qt::Key_Return, Qt::Key_Enter});
        km->setPassthroughKeys(std::move(passthrough));
        dashboard.setKeyboardMouseToggle([km]
                                         { km->toggle(); });
        auto* toggle_key = new QShortcut(QKeySequence(Qt::Key_F9), &dashboard);
        toggle_key->setContext(Qt::ApplicationShortcut);
        QObject::connect(toggle_key, &QShortcut::activated, km, [km]
                         { km->toggle(); });
        QObject::connect(km, &rm_terminal::KmCapture::activeChanged, &dashboard,
                         [km, self_commands, &dashboard](bool active, const QString& reason)
                         {
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
                         [km, &dashboard, &km_pause_reason](bool paused)
                         {
                             const bool died = km_pause_reason == QStringLiteral("阵亡");
                             rm_terminal::StructuredLog::write(
                                 rm_terminal::LogLevel::info, QStringLiteral("km_capture"),
                                 {QStringLiteral("state=%1").arg(paused ? "paused" : "resumed"),
                                  QStringLiteral("reason=%1")
                                      .arg(paused ? km_pause_reason
                                           : died ? QStringLiteral("复活")
                                                  : QStringLiteral("关掉") + km_pause_reason),
                                  QStringLiteral("frames=%1").arg(km->framesSent())});
                             dashboard.setKeyboardMouseStatus(
                                 !paused ? QStringLiteral("键鼠控制中（Esc 退出）")
                                 : died  ? QStringLiteral("键鼠控制暂停（阵亡，复活后自动继续）")
                                         : QStringLiteral("键鼠控制暂停（%1，关掉后自动继续）").arg(km_pause_reason));
                         });
    }
    // 阵亡、开着补弹面板或 P 设置面板时暂停键鼠控制、放开鼠标,复活框和面板才点得到;之后自动接着控制。
    bool self_dead = false;
    const auto pause_keyboard_mouse = [&keyboard_mouse, &km_pause_reason, &self_dead, purchases, settings_panel]
    {
        if (!keyboard_mouse)
            return;
        const bool supply = purchases && purchases->supplyOpen();
        const bool settings = settings_panel->isOpen();
        if (self_dead)
            km_pause_reason = QStringLiteral("阵亡");
        else if (supply)
            km_pause_reason = QStringLiteral("补弹面板");
        else if (settings)
            km_pause_reason = QStringLiteral("设置面板");
        keyboard_mouse->setSuspended(self_dead || supply || settings);
    };
    if (purchases)
        QObject::connect(purchases, &rm_terminal::PurchaseFlow::supplyOpenChanged, &dashboard,
                         [&pause_keyboard_mouse](bool)
                         { pause_keyboard_mouse(); });

    // 按住的键,照官方选手端:按住 Tab 看双方机器人(手册第 19 页),按住 ~ 看伤害和模块(第 20 页),
    // 步兵长按 F 一秒开能量机关(第 12 页)。F 同时照常发给车:队里的程序一般是短按 F 触发。
    auto* team_panel = new rm_terminal::TeamPanel(&dashboard);
    auto* injury_panel = new rm_terminal::InjuryPanel(&dashboard);
    auto* hold_keys = new rm_terminal::HoldKeyFilter(&app);
    app.installEventFilter(hold_keys);
    rm_terminal::RuneKey rune_key;
    QTimer rune_timer;
    rune_timer.setInterval(40);
    const auto rune_step = [&rune_key, &rune_timer, &dashboard, self_commands]
    {
        const rm_terminal::MonotonicMs now = rm_terminal::monotonic_now();
        if (self_commands && rune_key.due(now))
        {
            const bool ok = self_commands->openRune();
            rune_key.sent(ok, now);
            rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("rune_key"),
                                              {QStringLiteral("state=%1").arg(ok ? "sent" : "not_sent")});
        }
        const rm_terminal::RuneKey::Display shown = rune_key.display(now);
        dashboard.setRuneEntry(shown.visible ? shown.text : QString(), shown.lit, shown.progress, shown.tooltip);
        if (!rune_key.holding())
            rune_timer.stop();
    };
    QObject::connect(&rune_timer, &QTimer::timeout, &dashboard, rune_step);
    const auto rune_hold = [&rune_key, &rune_timer, &rune_step](bool down)
    {
        if (down)
        {
            rune_key.press(rm_terminal::monotonic_now());
            if (rune_key.holding())
                rune_timer.start();
        }
        else
        {
            rune_key.release();
        }
        rune_step();
    };
    QObject::connect(hold_keys, &rm_terminal::HoldKeyFilter::runeKeyChanged, &dashboard, rune_hold);
    dashboard.setRuneEntryHold(rune_hold);
    QObject::connect(hold_keys, &rm_terminal::HoldKeyFilter::tabChanged, &dashboard, [team_panel](bool down)
                     {
        team_panel->setHeld(down);
        rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("hold_panel"),
                                          {QStringLiteral("panel=tab"), QStringLiteral("shown=%1").arg(down ? 1 : 0)}); });
    QObject::connect(hold_keys, &rm_terminal::HoldKeyFilter::tildeChanged, &dashboard, [injury_panel](bool down)
                     {
        injury_panel->setHeld(down);
        rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("hold_panel"),
                                          {QStringLiteral("panel=tilde"), QStringLiteral("shown=%1").arg(down ? 1 : 0)}); });
    if (keyboard_mouse)
    {
        // 键鼠控制接管时 F 发给车,按键事件到不了上面的过滤器,由接管那边报 F 按下/松开。
        QObject::connect(keyboard_mouse.get(), &rm_terminal::KmCapture::keyChanged, &dashboard,
                         [&rune_hold](int key, bool down)
                         {
                             if (key == Qt::Key_F)
                                 rune_hold(down);
                         });
        // 接管开始、结束、暂停的那一下,F 的松开可能落在另一边:当作全松开,免得没按着也开了能量机关。
        const auto release_holds = [hold_keys, &rune_key, &rune_step]
        {
            hold_keys->releaseAll();
            rune_key.release();
            rune_step();
        };
        QObject::connect(keyboard_mouse.get(), &rm_terminal::KmCapture::activeChanged, &dashboard,
                         [release_holds](bool, const QString&)
                         { release_holds(); });
        QObject::connect(keyboard_mouse.get(), &rm_terminal::KmCapture::suspendedChanged, &dashboard,
                         [release_holds](bool)
                         { release_holds(); });
    }
    QObject::connect(settings_panel, &rm_terminal::SettingsPanel::openChanged, &dashboard,
                     [&pause_keyboard_mouse, &dashboard](bool open)
                     {
                         dashboard.setSettingsPanelOpen(open);
                         pause_keyboard_mouse();
                     });
    auto* settings_key = new QShortcut(QKeySequence(Qt::Key_P), &dashboard);
    settings_key->setContext(Qt::ApplicationShortcut);
    QObject::connect(settings_key, &QShortcut::activated, &dashboard, [settings_panel, purchases]
                     {
        if (purchases && !settings_panel->isOpen()) purchases->closeAll();
        settings_panel->toggle(); });
    // 赛前卡片上的「性能体系 · P」:和按 P 一样打开设置面板。
    dashboard.setPerformanceHandler([settings_panel, purchases]
                                    {
        if (purchases) purchases->closeAll();
        settings_panel->setOpen(true); });
    const auto apply_settings = [speech, &dashboard, &keyboard_mouse](const rm_terminal::TerminalSettings& settings)
    {
        speech->setVolume(settings.volume / 100.0);
        dashboard.setVideoRotated(settings.video_reversed);
        if (keyboard_mouse)
            keyboard_mouse->setSensitivity(rm_terminal::sensitivity_scale(settings.sensitivity));
    };
    apply_settings(terminal_settings);
    // 拖滑块时每一格都会改一次,停手 0.4 秒再存盘。
    QTimer settings_save;
    settings_save.setSingleShot(true);
    settings_save.setInterval(400);
    QObject::connect(&settings_save, &QTimer::timeout, &app, [&settings_path, &terminal_settings]
                     {
        QString error;
        if (!rm_terminal::save_terminal_settings(settings_path, terminal_settings, &error))
            rm_terminal::StructuredLog::write(rm_terminal::LogLevel::warning, QStringLiteral("terminal_settings"),
                                              {QStringLiteral("state=save_failed"), QStringLiteral("error=%1").arg(error)}); });
    const auto log_settings = [](const QString& what, const rm_terminal::TerminalSettings& settings)
    {
        rm_terminal::StructuredLog::write(
            rm_terminal::LogLevel::info, QStringLiteral("terminal_settings"),
            {QStringLiteral("change=%1").arg(what), QStringLiteral("sensitivity=%1").arg(settings.sensitivity),
             QStringLiteral("volume=%1").arg(settings.volume),
             QStringLiteral("video_reversed=%1").arg(settings.video_reversed ? 1 : 0)});
    };
    settings_panel->setActions(
        [&terminal_settings, &apply_settings, &settings_save, &log_settings](const rm_terminal::TerminalSettings& settings)
        {
            terminal_settings = settings;
            apply_settings(settings);
            settings_save.start();
            log_settings(QStringLiteral("set"), settings);
        },
        [self_commands](std::optional<std::uint32_t> shooter, std::optional<std::uint32_t> chassis)
        {
            if (self_commands)
                self_commands->choosePerformance(shooter, chassis);
        },
        [&dashboard](bool full)
        {
            if (full)
                dashboard.showFullScreen();
            else
                dashboard.showNormal();
            rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("display"),
                                              {QStringLiteral("state=%1").arg(full ? "fullscreen" : "window")});
        },
        [&terminal_settings, &apply_settings, &settings_save, &log_settings, settings_panel]
        {
            terminal_settings = rm_terminal::TerminalSettings{};
            settings_panel->setSettings(terminal_settings);
            apply_settings(terminal_settings);
            settings_save.start();
            log_settings(QStringLiteral("reset"), terminal_settings);
        });

    if (fullscreen)
        dashboard.showFullScreen();
    else
        dashboard.show();
    // 从 Terminal / Finder 脚本拉起时进程没有前台权限，窗口会藏在别的窗口后面，
    // 看起来像「双击没反应」。raise + activateWindow 把它顶到最前。
    dashboard.raise();
    dashboard.activateWindow();

    rm_terminal::StaleReporter reporter;
    // 链路灯和自检页只在实况下有意义:回放时没有 MQTT 也没有串口,亮一排红灯只会误导。
    struct LinkMeters
    {
        rm_terminal::RateMeter referee, video, controller, feedback;
        rm_terminal::LinkStateLog log;
    } meters;
    const auto sample_links = [&cfg, &intake, &video, &wall, &controller, &meters](
                                  rm_terminal::MonotonicMs now)
    {
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
        if (mqtt.last_custom_bytes_at)
        {
            car.freshness = now - *mqtt.last_custom_bytes_at < cfg.stale_window_ms
                                ? rm_terminal::Freshness::Fresh
                                : rm_terminal::Freshness::Stale;
            car.silent_ms = rm_terminal::silence(mqtt.last_custom_bytes_at, std::nullopt, now);
        }
        report.car = rm_terminal::car_status(car);

        if (!wall.empty())
        {
            std::vector<rm_terminal::WallLinkSample> feeds;
            for (const auto& [robot, receiver] : wall)
                feeds.push_back({rm_terminal::wall_label(robot), receiver->online()});
            report.video = rm_terminal::wall_status(feeds);
        }
        else
        {
            rm_terminal::VideoSample picture;
            picture.online = video.online();
            if (video.msSinceLastFrame() >= 0)
                picture.silent_ms = video.msSinceLastFrame();
            picture.frames_per_s =
                meters.video.update(static_cast<std::uint64_t>(video.decodedFrames()), now);
            report.video = rm_terminal::video_status(picture);
        }

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

    std::optional<bool> seat_matched;
    // 赛事事件和指挥手指令的回执也写进日志:复盘和验收脚本都靠它们看「服务器说了什么」。
    rm_terminal::MonotonicMs logged_event_at = -1;
    rm_terminal::MonotonicMs logged_sentry_result_at = -1;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> logged_dart;
    rm_terminal::MonotonicMs logged_mark_at = -1;
    // 上一帧的比赛阶段:进入自检阶段的那一下要提醒没选性能体系。
    std::optional<std::uint32_t> last_stage;
    // 本车的经济/发弹量/阵亡、能量机关、性能体系,变了才写一行:验收脚本靠它看
    // 兑换、复活、开能量机关、选性能体系有没有被服务器执行,复盘也看得到。
    struct SelfLog
    {
        QString supply, rune, performance, exchange, purchase;
    } self_log;
    QTimer freshness_timer;
    QObject::connect(&freshness_timer, &QTimer::timeout,
                     [&store, &reporter, &dashboard, &video, &wall, &reminders, &intake,
                      &recorder, &replay, &sample_links, &cfg, &recordings, speech,
                      &match_alerts, &alert_voice, &alert_voice_on, assembly, dart, sentry,
                      &seat_matched, &logged_event_at, &logged_sentry_result_at,
                      &logged_dart, &logged_mark_at, self_commands, revive_popup, &keyboard_mouse,
                      &self_log, purchases, &self_dead, &pause_keyboard_mouse, settings_panel, seat_role,
                      &performance_phrase, &last_stage, &rune_key, &rune_step, team_panel, injury_panel,
                      &match_log, &reports]()
                     {
                         // One clock read per tick, shared by the snapshot and the mode machine:
                         // two reads would hand the machine an instant the snapshot never saw.
                         const rm_terminal::MonotonicMs now = rm_terminal::monotonic_now();
                         const auto snapshot = store.snapshot(now);
                         reporter.inspect(snapshot);
                         const auto events = snapshot.events.recent(snapshot.events.size());
                         for (auto it = events.rbegin(); it != events.rend(); ++it)
                         {
                             if (it->received_at <= logged_event_at)
                                 continue;
                             rm_terminal::StructuredLog::write(
                                 rm_terminal::LogLevel::info, QStringLiteral("match_event"),
                                 {QStringLiteral("id=%1").arg(it->event_id), QStringLiteral("level=%1").arg(it->level),
                                  QStringLiteral("text=%1").arg(QString::fromStdString(it->text))});
                         }
                         if (!events.empty())
                             logged_event_at = std::max(logged_event_at, events.front().received_at);
                         if (snapshot.sentry_result && snapshot.sentry_result->received_at > logged_sentry_result_at)
                         {
                             logged_sentry_result_at = snapshot.sentry_result->received_at;
                             rm_terminal::StructuredLog::write(
                                 rm_terminal::LogLevel::info, QStringLiteral("sentry_result"),
                                 {QStringLiteral("command=%1").arg(snapshot.sentry_result->command_id),
                                  QStringLiteral("code=%1").arg(snapshot.sentry_result->result_code)});
                         }
                         for (const auto& mark : snapshot.map_marks)
                         {
                             if (mark.received_at <= logged_mark_at)
                                 continue;
                             logged_mark_at = mark.received_at;
                             rm_terminal::StructuredLog::write(
                                 rm_terminal::LogLevel::info, QStringLiteral("map_mark_received"),
                                 {QStringLiteral("kind=%1").arg(mark.mode), QStringLiteral("target=%1").arg(mark.type),
                                  QStringLiteral("enemy=%1").arg(mark.enemy_id),
                                  QStringLiteral("x=%1").arg(mark.x, 0, 'f', 2), QStringLiteral("y=%1").arg(mark.y, 0, 'f', 2)});
                         }
                         if (snapshot.dart.target_id.value && snapshot.dart.gate.value)
                         {
                             const std::pair<std::uint32_t, std::uint32_t> dart_now{*snapshot.dart.target_id.value,
                                                                                    *snapshot.dart.gate.value};
                             if (logged_dart != dart_now)
                                 rm_terminal::StructuredLog::write(
                                     rm_terminal::LogLevel::info, QStringLiteral("dart_status"),
                                     {QStringLiteral("target=%1").arg(dart_now.first),
                                      QStringLiteral("gate=%1").arg(dart_now.second)});
                             logged_dart = dart_now;
                         }
                         match_log.setRecordingMs(recorder.elapsedMs());
                         const bool round_over = match_log.observe(snapshot);
                         for (const auto& alert : match_alerts.observe(snapshot, now))
                         {
                             rm_terminal::StructuredLog::write(
                                 rm_terminal::LogLevel::info, QStringLiteral("alert"),
                                 {QStringLiteral("kind=%1").arg(rm_terminal::alert_kind_name(alert.kind)),
                                  QStringLiteral("level=%1").arg(rm_terminal::alert_level_name(alert.level)),
                                  QStringLiteral("text=%1").arg(alert.banner)});
                             dashboard.showAlert(alert.banner, alert.level, now);
                             match_log.note(alert.banner);
                             if (alert.kind == rm_terminal::AlertKind::BaseAttacked)
                                 dashboard.flashBase(now);
                             if (!replay && alert_voice_on)
                                 reminders.announce(alert_voice.pathFor(alert.speech));
                         }
                         if (round_over && !replay && snapshot.self)
                         {
                             rm_terminal::ReportContext context;
                             context.self_id = snapshot.self->value;
                             context.recording = recorder.active() ? recorder.path() : QString();
                             context.finished_at = QDateTime::currentDateTime();
                             context.pdf = true;
                             QString report_error;
                             const QString report = rm_terminal::write_match_report(reports, match_log, context, &report_error);
                             rm_terminal::StructuredLog::write(
                                 report.isEmpty() ? rm_terminal::LogLevel::warning : rm_terminal::LogLevel::info,
                                 QStringLiteral("match_report"),
                                 {report.isEmpty() ? QStringLiteral("error=%1").arg(report_error) : QStringLiteral("path=%1").arg(report)});
                             if (!report.isEmpty())
                                 qInfo().noquote() << "match report:" << report;
                         }
                         dashboard.update(snapshot, &video, now);
                         if (!wall.empty())
                         {
                             std::vector<rm_terminal::WallFeed> feeds;
                             for (const auto& [robot, receiver] : wall)
                                 feeds.push_back(rm_terminal::wall_feed(robot, receiver.get()));
                             dashboard.setWallFeeds(std::move(feeds));
                         }
                         // 「下一步」第 1 件换了就念一句(固定句子,赛前合成好的)。
                         if (const QString phrase = dashboard.takeNextSpeech(); !phrase.isEmpty() && !replay)
                             reminders.announce(alert_voice.pathFor(phrase));
                         dashboard.setRecordingStatus(replay ? replay->statusText() : recorder.statusText());
                         const bool link_up = !replay && intake.connected();
                         dashboard.setLinkUp(link_up);
                         if (dart)
                         {
                             dart->setView(rm_terminal::dart_view(snapshot));
                             dart->setLinkUp(link_up);
                             dart->tick(now);
                         }
                         if (sentry)
                         {
                             sentry->setView(rm_terminal::sentry_view(snapshot, &dashboard.buffBoard()));
                             sentry->setLinkUp(link_up);
                             sentry->tick(now);
                             dashboard.setPaneHeader(rm_terminal::PaneId::Sentry, sentry->headerText());
                         }
                         bool performance_untouched = false;
                         QString video_status;
                         if (self_commands)
                         {
                             const rm_terminal::SelfCommandView view = rm_terminal::self_command_view(snapshot);
                             rune_key.update(view, now);
                             rune_step();
                             self_commands->setView(view);
                             self_commands->setLinkUp(link_up);
                             self_commands->tick(now);
                             const auto text = [](const auto& value)
                             {
                                 return value ? QString::number(*value) : QStringLiteral("--");
                             };
                             const auto log_change = [](QString& last, const QString& now_text, const char* event)
                             {
                                 if (now_text == last)
                                     return;
                                 last = now_text;
                                 rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QString::fromLatin1(event),
                                                                   {now_text});
                             };
                             const auto flag = [](const std::optional<bool>& value)
                             {
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
                             // 兑换的前提(兑换点、发射机构)和兑换到没到账:验收脚本和复盘看终端判断得对不对。
                             log_change(self_log.exchange,
                                        QStringLiteral("point=%1 shooter=%2 remote_ammo=%3 remote_heal=%4")
                                            .arg(flag(view.at_exchange_point), flag(view.shooter_online),
                                                 flag(view.can_remote_ammo), flag(view.can_remote_heal)),
                                        "exchange_state");
                             self_dead = view.dead.value_or(false);
                             if (assembly)
                             {
                                 rm_terminal::AssemblyState::Inputs inputs;
                                 inputs.view = rm_terminal::assembly_view(snapshot);
                                 inputs.link_up = link_up;
                                 inputs.in_match = view.stage == std::optional<std::uint32_t>(4);
                                 inputs.dead = self_dead;
                                 assembly->update(inputs, now);
                             }
                             if (purchases)
                             {
                                 purchases->update(view, link_up, now);
                                 self_commands->setPurchaseText(purchases->statusText(), purchases->statusProblem());
                                 const QString result = purchases->statusText();
                                 if (!result.isEmpty())
                                     log_change(self_log.purchase, QStringLiteral("text=%1").arg(result), "purchase");
                             }
                             // 性能体系(只有步兵):准备到倒计时阶段还是默认,比分上方一直挂提醒;进入 15 秒自检
                             // 阶段时还没选,再弹一次横幅并念一句。终端分不出「没选」和「在官方选手端选了默认」,
                             // 所以提醒写的是「还是默认」。
                             performance_untouched = view.infantry && !self_commands->performanceChosen() &&
                                                     self_commands->shownShooter().value_or(1) == 1 &&
                                                     self_commands->shownChassis().value_or(1) == 1;
                             const bool choosing = view.stage && *view.stage >= 1 && *view.stage <= 3;
                             dashboard.setPreMatchReminder(
                                 choosing && performance_untouched
                                     ? QStringLiteral("性能体系还是默认（血量优先 + 冷却优先）· 按 P 修改，开赛就锁定")
                                     : QString());
                             // 赛前卡片上「性能体系 · P」按钮的第二行:现在选的是什么。
                             QString performance_text;
                             if (view.infantry)
                             {
                                 const QString chassis = self_commands->shownChassis().value_or(1) == 2 ? QStringLiteral("功率优先")
                                                                                                        : QStringLiteral("血量优先");
                                 const QString shooter = self_commands->shownShooter().value_or(1) == 2 ? QStringLiteral("爆发优先")
                                                                                                        : QStringLiteral("冷却优先");
                                 performance_text = !choosing               ? QStringLiteral("准备阶段开始后才能选")
                                                    : performance_untouched ? QStringLiteral("还是默认，开赛就锁定")
                                                                            : chassis + QStringLiteral(" + ") + shooter;
                             }
                             dashboard.setPreMatchPerformance(performance_text, choosing && performance_untouched);
                             if (performance_untouched && view.stage == std::optional<std::uint32_t>(2) && last_stage != view.stage)
                             {
                                 dashboard.showAlert(QStringLiteral("还没选性能体系：按 P 选，开赛就锁定"),
                                                     rm_terminal::AlertLevel::Warning, now);
                                 rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("alert"),
                                                                   {QStringLiteral("kind=performance_default"),
                                                                    QStringLiteral("level=warning")});
                                 if (!replay && alert_voice_on)
                                     reminders.announce(alert_voice.pathFor(performance_phrase));
                             }
                             last_stage = view.stage;
                             if (revive_popup)
                             {
                                 revive_popup->setWindowActive(dashboard.isActiveWindow());
                                 revive_popup->setLinkUp(link_up);
                                 revive_popup->setStatus(self_commands->reviveEchoText());
                                 revive_popup->setView(view);
                             }
                             if (keyboard_mouse)
                             {
                                 keyboard_mouse->setLinkUp(link_up);
                                 pause_keyboard_mouse();
                                 QStringList held;
                                 const QString keys = rm_terminal::km_keys_text(keyboard_mouse->keysDown());
                                 if (!keys.isEmpty())
                                     held << keys;
                                 if (keyboard_mouse->leftDown())
                                     held << QStringLiteral("左键");
                                 if (keyboard_mouse->rightDown())
                                     held << QStringLiteral("右键");
                                 self_commands->setKeyboardMouseState(keyboard_mouse->active() && !keyboard_mouse->suspended(),
                                                                      held.join(QLatin1Char(' ')));
                             }
                         }
                         if (!replay)
                         {
                             rm_terminal::SelfCheckInputs inputs;
                             inputs.self_robot_id = cfg.self_robot_id;
                             // 席位核对:服务器 RobotStaticStatus 报的编号和开机选的不一致,说明登录名填错。
                             const auto self = snapshot.robots.find(
                                 rm_terminal::RobotId{static_cast<std::uint32_t>(cfg.self_robot_id)});
                             if (self != snapshot.robots.end() &&
                                 self->second.statics.robot_id.quality == rm_terminal::Quality::Valid)
                                 inputs.server_robot_id = self->second.statics.robot_id.value;
                             if (inputs.server_robot_id)
                             {
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
                             if (disk.isValid())
                                 inputs.free_bytes = disk.bytesAvailable();
                             inputs.speech_available = speech->available();
                             inputs.speech_hint = speech->unavailableReason();
                             dashboard.setLinkReport(inputs.links);
                             dashboard.setSelfCheck(rm_terminal::build_self_check(inputs));
                             video_status = inputs.links.video.detail.isEmpty()
                                                ? rm_terminal::link_level_name(inputs.links.video.level)
                                                : inputs.links.video.detail;
                         }
                         // P 设置面板每帧刷新:登录、性能设置、全屏与否、图传状态。
                         rm_terminal::SettingsView settings_view;
                         settings_view.seat = rm_terminal::self_robot_label(cfg.self_robot_id);
                         if (replay)
                         {
                             settings_view.login = QStringLiteral("回放录像，不连服务器");
                         }
                         else if (!link_up)
                         {
                             settings_view.login = QStringLiteral("没连上裁判系统服务器");
                             settings_view.login_problem = true;
                         }
                         else if (seat_matched == std::optional<bool>(false))
                         {
                             settings_view.login = QStringLiteral("服务器报的编号和开机选的不一样，席位选错了");
                             settings_view.login_problem = true;
                         }
                         else if (seat_matched == std::optional<bool>(true))
                         {
                             settings_view.login = QStringLiteral("已登录 · 服务器核对过编号");
                         }
                         else
                         {
                             settings_view.login = QStringLiteral("已连上，等服务器核对编号");
                         }
                         const auto& stage_field = snapshot.game.current_stage;
                         if (stage_field.quality == rm_terminal::Quality::Valid)
                             settings_view.stage = stage_field.value;
                         settings_view.has_performance = self_commands && seat_role == rm_terminal::Role::Infantry;
                         if (!settings_view.has_performance)
                         {
                             settings_view.performance_note = seat_role == rm_terminal::Role::Hero
                                                                  ? QStringLiteral("重装没有性能体系")
                                                                  : QStringLiteral("这个席位没有性能体系");
                         }
                         else
                         {
                             settings_view.performance_open = self_commands->performanceOpen();
                             settings_view.shooter = self_commands->shownShooter();
                             settings_view.chassis = self_commands->shownChassis();
                             const auto stage = settings_view.stage;
                             if (!stage)
                             {
                                 settings_view.performance_note = QStringLiteral("还没收到比赛阶段");
                             }
                             else if (*stage == 0)
                             {
                                 settings_view.performance_note = QStringLiteral("准备阶段开始后才能选");
                             }
                             else if (*stage <= 3)
                             {
                                 settings_view.performance_warn = performance_untouched;
                                 settings_view.performance_note = performance_untouched
                                                                      ? QStringLiteral("还是默认，开赛就锁定成血量优先 + 冷却优先")
                                                                      : QStringLiteral("准备、自检阶段能改，开赛锁定");
                                 if (const QString echo = self_commands->performanceEcho(); !echo.isEmpty())
                                     settings_view.performance_note += QStringLiteral(" · ") + echo;
                             }
                             else
                             {
                                 settings_view.performance_note = QStringLiteral("比赛中已锁定");
                             }
                         }
                         settings_view.fullscreen = dashboard.isFullScreen();
                         settings_view.volume_adjustable = speech->volumeAdjustable();
                         settings_view.video = replay ? QStringLiteral("回放录像") : video_status;
                         settings_panel->setView(settings_view);
                         // 上面的 update()、setSelfCheck() 会把自检页、比赛弹窗提到最上面;操作手自己按键打开的框
                         // 要盖在它们上面才点得到。放在这一拍最后。
                         if (purchases)
                             purchases->raiseOpen();
                         if (assembly)
                             assembly->raiseOpen();
                         if (settings_panel->isOpen())
                             settings_panel->raise();
                         team_panel->setView(rm_terminal::team_view(snapshot));
                         injury_panel->setView(rm_terminal::injury_view(snapshot));
                         if (team_panel->isVisible())
                             team_panel->raise();
                         if (injury_panel->isVisible())
                             injury_panel->raise();
                         // 同一个 now 和同一份快照喂给提醒:另取一次时钟会让调度看到一个快照
                         // 从未见过的时刻,而两遍之间的间隔正是按这个时刻算的。
                         reminders.observe(rm_terminal::reminder_inputs(snapshot, intake.connected()), now);
                     });
    freshness_timer.start(250);

    // 图传按新帧刷新，不跟 250ms 的快照节拍走；没有新帧时 refreshVideo 自己跳过。
    QTimer video_timer;
    video_timer.setInterval(15);
    QObject::connect(&video_timer, &QTimer::timeout, [&dashboard, &video, &replay]()
                     {
        if (!replay) dashboard.refreshVideo(&video); });
    video_timer.start();

    // 第二遍的到点检查。250ms 的快照节拍对 1 秒的间隔来说太粗:间隔期满的时刻
    // 落在两次快照之间时,第二遍会被推迟到下一个快照。这个定时器只推进播放队列,
    // 不碰调度状态,所以跑得比快照密不会影响任何时序判定。
    QTimer reminder_timer;
    QObject::connect(&reminder_timer, &QTimer::timeout, [&reminders]()
                     { reminders.tick(rm_terminal::monotonic_now()); });
    reminder_timer.start(100);

    // Evidence capture renders the widget itself rather than grabbing the
    // screen: it needs no recording permission, captures nothing but this
    // window, and stays deterministic in the offscreen Qt platform plugin.
    int capture_status = rm_terminal::kExitSuccess;

    // 片段放到 --to 就停;导 MP4 时每 1/15 秒把整个窗口画一帧喂给 ffmpeg,到点收尾退出。
    QProcess encoder;
    QSize encoder_size;
    QTimer clip_timer;
    if (replay && clip_to_s)
    {
        QObject::connect(&clip_timer, &QTimer::timeout, &app, [&]()
                         {
            const bool done = replay->finished() || replay->positionMs() >= static_cast<qint64>(*clip_to_s * 1000);
            if (!export_video.isEmpty()) {
                const QImage frame = dashboard.grab().toImage().convertToFormat(QImage::Format_ARGB32);
                if (encoder_size.isEmpty()) {
                    encoder_size = frame.size();
                    encoder.start(QStringLiteral("ffmpeg"),
                                  {QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                                   QStringLiteral("-f"), QStringLiteral("rawvideo"), QStringLiteral("-pix_fmt"),
                                   QStringLiteral("bgra"), QStringLiteral("-s"),
                                   QStringLiteral("%1x%2").arg(frame.width()).arg(frame.height()), QStringLiteral("-r"),
                                   QStringLiteral("15"), QStringLiteral("-i"), QStringLiteral("-"), QStringLiteral("-c:v"),
                                   QStringLiteral("libx264"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                                   QStringLiteral("-preset"), QStringLiteral("veryfast"), export_video});
                    if (!encoder.waitForStarted(5000)) {
                        qCritical().noquote() << "export failed: ffmpeg did not start";
                        clip_timer.stop();
                        app.exit(rm_terminal::kExitEvidenceFailure);
                        return;
                    }
                }
                if (frame.size() == encoder_size)
                    encoder.write(reinterpret_cast<const char*>(frame.constBits()), frame.sizeInBytes());
            }
            if (!done) return;
            clip_timer.stop();
            if (export_video.isEmpty()) {
                if (!replay->paused()) replay->togglePause();
                return;
            }
            encoder.closeWriteChannel();
            encoder.waitForFinished(120000);
            const bool ok = encoder.exitStatus() == QProcess::NormalExit && encoder.exitCode() == 0;
            if (ok) qInfo().noquote() << "video written:" << export_video;
            else qCritical().noquote() << "export failed:" << QString::fromLocal8Bit(encoder.readAllStandardError());
            app.exit(ok ? rm_terminal::kExitSuccess : rm_terminal::kExitEvidenceFailure); });
        clip_timer.start(1000 / 15);
    }
    if (!screenshot_path.isEmpty() || !dump_layout_path.isEmpty())
    {
        // Video needs longer than the widget itself: the decoder must spawn and
        // fill one frame, so capturing at 600ms would always show an empty pane.
        const int capture_delay_ms =
            qEnvironmentVariableIntValue("RM_TERMINAL_CAPTURE_DELAY_MS") > 0
                ? qEnvironmentVariableIntValue("RM_TERMINAL_CAPTURE_DELAY_MS")
                : 600;
        QTimer::singleShot(capture_delay_ms, &app, [&]()
                           {
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
            app.quit(); });
    }

    // The shutdown record must carry the code this process actually returns.
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     [&capture_status]()
                     {
                         log_shutdown(QStringLiteral("terminal"), capture_status);
                     });
    const int result = app.exec();
    return capture_status != rm_terminal::kExitSuccess ? capture_status : result;
}
