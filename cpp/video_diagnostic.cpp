#include "video_receiver.h"
#include "logging.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QTextStream>

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    QCommandLineParser parser;parser.addHelpOption();
    parser.addOption({"bind","Local UDP address","address","127.0.0.1"});
    parser.addOption({"port","Local UDP port (0 selects an owned ephemeral port)","port","0"});
    parser.addOption({"duration","Bounded duration in seconds (1..60)","seconds","10"});
    parser.addOption({"ffmpeg","FFmpeg executable name or path","path","ffmpeg"});
    parser.addOption({"log","Structured log destination; empty disables logging","path",""});
    parser.process(app);
    bool port_ok=false,duration_ok=false;
    const int port=parser.value("port").toInt(&port_ok);
    const int duration=parser.value("duration").toInt(&duration_ok);
    const QHostAddress address(parser.value("bind"));
    if(!parser.positionalArguments().isEmpty() || !port_ok || port<0 || port>65535 ||
        !duration_ok || duration<1 || duration>60 || address.isNull())return 2;
    const auto log_path=parser.value("log");
    if(!log_path.isEmpty()) {
        QString error;
        if(!rm_terminal::StructuredLog::open(log_path,rm_terminal::LogLevel::debug,&error)) {
            QTextStream(stderr)<<"log error: "<<error<<Qt::endl;
            return 4;
        }
        rm_terminal::set_log_sink([](rm_terminal::LogLevel level,const QString& event,
                                     const QStringList& fields) {
            rm_terminal::StructuredLog::write(level,event,fields);
        });
    }
    // Detaches the sink before closing the file on every return path, including
    // the early bind failure, so no queued record can write to a closed QFile.
    struct LogGuard {
        ~LogGuard() {
            rm_terminal::set_log_sink(nullptr);
            rm_terminal::StructuredLog::close();
        }
    } log_guard;
    rm_terminal::VideoReceiver receiver;
    const auto report=[&] {
        auto state=receiver.snapshot();state.insert("pid",app.applicationPid());
        QTextStream(stdout)<<QJsonDocument(state).toJson(QJsonDocument::Compact)<<Qt::endl;
    };
    if(!receiver.start({address,quint16(port),parser.value("ffmpeg")})){report();return 3;}
    report();QTimer timer;
    QObject::connect(&timer,&QTimer::timeout,&app,report);timer.start(200);
    QTimer::singleShot(duration*1000,&app,&QCoreApplication::quit);
    app.exec();receiver.stop();report();
    return receiver.snapshot()["decoded_frames"].toInteger()>0?0:4;
}
