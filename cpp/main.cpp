#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QLabel>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <cstdio>
#include <optional>

#include "clock.h"
#include "config.h"
#include "dashboard.h"
#include "logging.h"
#include "mqtt_intake.h"
#include "platform.h"
#include "presentation.h"
#include "store.h"
#include "ui_mode.h"
#include "video_receiver.h"

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

    rm_terminal::Dashboard dashboard(cfg);
    dashboard.setWindowTitle("RM Terminal");
    dashboard.setMinimumSize(980, 620);
    // Applied before the first update() so the forced layout holds from the very
    // first tick; injecting it afterwards would render one automatic frame first
    // and put the wrong layout in a capture taken at a short delay.
    if (forced_mode.has_value()) dashboard.forceMode(forced_mode);
    const rm_terminal::MonotonicMs startup_now = rm_terminal::monotonic_now();
    dashboard.update(store.snapshot(startup_now), &video, startup_now);
    dashboard.show();

    rm_terminal::StaleReporter reporter;
    QTimer freshness_timer;
    QObject::connect(&freshness_timer, &QTimer::timeout, [&store, &reporter, &dashboard, &video]() {
        // One clock read per tick, shared by the snapshot and the mode machine:
        // two reads would hand the machine an instant the snapshot never saw.
        const rm_terminal::MonotonicMs now = rm_terminal::monotonic_now();
        const auto snapshot = store.snapshot(now);
        reporter.inspect(snapshot);
        dashboard.update(snapshot, &video, now);
    });
    freshness_timer.start(250);

    // Evidence capture renders the widget itself rather than grabbing the
    // screen: it needs no recording permission, captures nothing but this
    // window, and stays deterministic in the offscreen Qt platform plugin.
    int capture_status = rm_terminal::kExitSuccess;
    if (!screenshot_path.isEmpty()) {
        // Video needs longer than the widget itself: the decoder must spawn and
        // fill one frame, so capturing at 600ms would always show an empty pane.
        const int capture_delay_ms =
            qEnvironmentVariableIntValue("RM_TERMINAL_CAPTURE_DELAY_MS") > 0
                ? qEnvironmentVariableIntValue("RM_TERMINAL_CAPTURE_DELAY_MS")
                : 600;
        QTimer::singleShot(capture_delay_ms, &app, [&]() {
            const QPixmap frame = dashboard.grab();
            if (frame.isNull() || !frame.save(screenshot_path, "PNG")) {
                qCritical().noquote() << "screenshot failed:" << screenshot_path;
                capture_status = rm_terminal::kExitEvidenceFailure;
            } else {
                qInfo().noquote() << "screenshot written:" << screenshot_path
                                  << frame.width() << "x" << frame.height();
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
