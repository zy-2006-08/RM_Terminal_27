#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <memory>
#include <cstdio>
#include <optional>
#include <string>

#include "clock.h"
#include "config.h"
#include "dashboard.h"
#include "logging.h"
#include "mqtt_intake.h"
#include "platform.h"
#include "presentation.h"
#include "reminder_repository.h"
#include "store.h"
#include "tactical_reminder_controller.h"
#include "ui_mode.h"
#include "video_receiver.h"

#if defined(RM_TERMINAL_HAS_SPEECH)
#include "macos_speech_backend.h"
#endif

namespace {

bool prepare_runtime(const QString& config_path, rm_terminal::Config* cfg) {
    QString error;
    if (!rm_terminal::load_config(config_path, cfg, &error)) {
        fprintf(stderr, "config error: %s\n", error.toUtf8().constData());
        return false;
    }
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

void log_shutdown(const QString& mode, int code) {
    rm_terminal::StructuredLog::write(
        rm_terminal::LogLevel::info, QStringLiteral("shutdown"),
        {QStringLiteral("mode=%1").arg(mode), QStringLiteral("exit=%1").arg(code)});
    rm_terminal::set_log_sink(nullptr);
    rm_terminal::StructuredLog::close();
}

// 语音的平台边界收在这一个函数里,上层不再有 #if。
rm_terminal::SpeechBackend* make_speech_backend(QObject* parent) {
#if defined(RM_TERMINAL_HAS_SPEECH)
    return new rm_terminal::MacosSpeechBackend(parent);
#else
    return new rm_terminal::UnavailableSpeechBackend(
        QStringLiteral("当前平台没有已核实的本机中文离线语音，战术提醒无法播报"), parent);
#endif
}

QStringList startup_fields(const QString& mode, const rm_terminal::Config& cfg) {
    return {QStringLiteral("mode=%1").arg(mode),
            QStringLiteral("platform=%1").arg(QString::fromLatin1(rm_terminal::platform_name())),
            QStringLiteral("mqtt_host=%1").arg(cfg.mqtt_host),
            QStringLiteral("mqtt_port=%1").arg(cfg.mqtt_port),
            QStringLiteral("udp_port=%1").arg(cfg.udp_port),
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

    if (safe_smoke) {
        QCoreApplication app(argc, argv);
        qInfo().noquote() << "RM terminal: READ-ONLY SIMULATION SAFE; platform="
                          << rm_terminal::platform_name();
        rm_terminal::Config cfg;
        if (!prepare_runtime(config_path, &cfg)) return rm_terminal::kExitConfigError;
        rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("startup"),
                                          startup_fields(QStringLiteral("safe_smoke"), cfg));
        rm_terminal::StructuredLog::write(
            rm_terminal::LogLevel::warning, QStringLiteral("readonly_block"),
            {QStringLiteral("reason=safe_smoke"), QStringLiteral("transport=none")});
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
        if (!prepare_runtime(config_path, &cfg)) return rm_terminal::kExitConfigError;
        rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("startup"),
                                          startup_fields(QStringLiteral("diagnostic"), cfg));
        rm_terminal::Store store(cfg.stale_window_ms);
        rm_terminal::MqttIntake intake(store, args.at(1), port);
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
                          << "event.text=" << static_cast<int>(snapshot.event.text.quality);
        log_shutdown(QStringLiteral("diagnostic"), result);
        return result;
    }

    if (!args.isEmpty()) {
        qCritical().noquote() << "Unknown argument; supported: --safe-smoke, --config <path>,"
                                 " --screenshot <path>, --force-mode <info|video>,"
                                 " --dump-layout <path>,"
                                 " --diagnostic <host> <port> <seconds>";
        return rm_terminal::kExitBadArguments;
    }

    QApplication app(argc, argv);
    rm_terminal::Config cfg;
    if (!prepare_runtime(config_path, &cfg)) return rm_terminal::kExitConfigError;
    rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("startup"),
                                      startup_fields(QStringLiteral("terminal"), cfg));

    rm_terminal::Store store(cfg.stale_window_ms);
    rm_terminal::MqttIntake intake(store, cfg.mqtt_host, cfg.mqtt_port);
    QObject::connect(&intake, &rm_terminal::MqttIntake::diagnostic,
                     [](const QString& text) { qInfo().noquote() << text; });
    if (!intake.start()) {
        rm_terminal::StructuredLog::write(
            rm_terminal::LogLevel::warning, QStringLiteral("reconnect_mqtt"),
            {QStringLiteral("state=unavailable"),
             QStringLiteral("host=%1").arg(cfg.mqtt_host),
             QStringLiteral("port=%1").arg(cfg.mqtt_port)});
        rm_terminal::StructuredLog::write(
            rm_terminal::LogLevel::warning, QStringLiteral("readonly_block"),
            {QStringLiteral("reason=mqtt_unavailable"), QStringLiteral("transport=none")});
    }

    rm_terminal::VideoReceiver video;
    rm_terminal::VideoEndpoint endpoint;
    endpoint.port = static_cast<quint16>(cfg.udp_port);
    if (!video.start(endpoint)) {
        rm_terminal::StructuredLog::write(
            rm_terminal::LogLevel::warning, QStringLiteral("reconnect_udp"),
            {QStringLiteral("state=start_failed"),
             QStringLiteral("port=%1").arg(cfg.udp_port)});
    }

    // 语音后端挂在 app 上,生命周期覆盖整个进程:控制器只借用它,不拥有它。
    auto* speech = make_speech_backend(&app);
    rm_terminal::ReminderController reminders(
        std::make_unique<rm_terminal::ReminderRepository>(
            rm_terminal::ReminderRepository::defaultDirectory()),
        speech, cfg.stale_window_ms);
    {
        // 音频缓存和配置分开放:缓存是可再生的派生物,清掉只会重新合成;配置是
        // 操作手赛前填的战术,清掉就没了。混在一个目录里迟早会被一起删。
        const auto cache_dir =
            QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
                .filePath(QStringLiteral("reminders"));
        const auto error = reminders.start(cache_dir);
        if (!error.isEmpty())
            rm_terminal::StructuredLog::write(rm_terminal::LogLevel::warning,
                                              QStringLiteral("reminder_config"),
                                              {QStringLiteral("state=invalid"),
                                               QStringLiteral("error=%1").arg(error)});
    }

    rm_terminal::Dashboard dashboard(cfg);
    dashboard.attachReminders(&reminders);
    dashboard.setWindowTitle("RM Terminal");
    // Four columns plus the flanking roster cards need more width than the old
    // two-column layout; below this the map degrades to a sliver.
    dashboard.setMinimumSize(1280, 760);
    dashboard.resize(1440, 900);
    // Applied before the first update() so the forced layout holds from the very
    // first tick; injecting it afterwards would render one automatic frame first
    // and put the wrong layout in a capture taken at a short delay.
    if (forced_mode.has_value()) dashboard.forceMode(forced_mode);
    const rm_terminal::MonotonicMs startup_now = rm_terminal::monotonic_now();
    dashboard.update(store.snapshot(startup_now), &video, startup_now);
    dashboard.show();

    rm_terminal::StaleReporter reporter;
    QTimer freshness_timer;
    QObject::connect(&freshness_timer, &QTimer::timeout,
                     [&store, &reporter, &dashboard, &video, &reminders, &intake]() {
        // One clock read per tick, shared by the snapshot and the mode machine:
        // two reads would hand the machine an instant the snapshot never saw.
        const rm_terminal::MonotonicMs now = rm_terminal::monotonic_now();
        const auto snapshot = store.snapshot(now);
        reporter.inspect(snapshot);
        dashboard.update(snapshot, &video, now);
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
