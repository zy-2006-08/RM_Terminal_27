#include "clock.h"
#include "recorder.h"
#include "replay_player.h"
#include "rm_terminal.pb.h"
#include "store.h"
#include "video_receiver.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <cstdlib>

namespace {

void check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "replay_player_test: FAILED: %s\n", what);
    std::exit(1);
}

void process_for(int milliseconds) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    QTemporaryDir directory;
    check(directory.isValid(), "temporary directory");

    rm_terminal::Recorder recorder;
    rm_terminal::RecordingMetadata metadata;
    metadata.self_robot_id = 4;
    QString error;
    check(recorder.start(directory.path(), metadata, &error), "recorder start");

    const auto publish = [&recorder](int score) {
        rm::GameStatus status;
        status.set_current_round(1);
        status.set_red_score(score);
        recorder.recordMqtt(
            QStringLiteral("GameStatus"),
            QByteArray::fromStdString(status.SerializeAsString()));
    };
    publish(3);
    QThread::msleep(300);
    publish(7);
    QThread::msleep(300);
    publish(9);
    recorder.stop();
    const QString path = recorder.path();

    rm_terminal::Store store(3000);
    rm_terminal::VideoReceiver video;
    rm_terminal::ReplayPlayer player(store, video);
    check(player.start(path, &error), "player start");
    const qint64 duration = player.durationMs();
    check(duration >= 550, "duration covers the recorded span");

    process_for(150);
    check(player.positionMs() > 0, "playhead advances");
    check(store.snapshot(rm_terminal::monotonic_now()).game.red_score.value == 3,
          "first record applied");

    player.togglePause();
    const qint64 frozen = player.positionMs();
    process_for(200);
    check(player.paused(), "pause flag");
    check(player.positionMs() == frozen, "pause freezes the playhead");

    player.seek(350);
    check(player.positionMs() == 350, "seek lands on the requested time");
    check(player.paused(), "seek preserves pause");
    check(store.snapshot(rm_terminal::monotonic_now()).game.red_score.value == 7,
          "seek rebuilds state only up to the target");

    player.seek(duration);
    check(store.snapshot(rm_terminal::monotonic_now()).game.red_score.value == 9,
          "seeking to the end applies every record");

    player.setSpeed(4.0);
    check(player.speed() == 4.0, "speed is stored");
    player.seek(0);
    player.togglePause();
    process_for(500);
    check(player.finished(), "4x playback reaches the end");

    player.togglePause();
    check(player.positionMs() < 20, "play after finish restarts from zero");
    player.stop();

    // 终端被强杀时文件尾会留下半条记录。完整的部分必须照常回放。
    QFile file(path);
    check(file.open(QIODevice::Append), "can append a partial record");
    file.write("12345", 5);
    file.close();

    rm_terminal::Store cut_store(3000);
    rm_terminal::ReplayPlayer cut(cut_store, video);
    check(cut.start(path, &error), "recording with a truncated tail still starts");
    check(cut.durationMs() == duration, "duration ends at the last complete record");
    cut.setSpeed(4.0);
    process_for(500);
    check(cut.finished(), "truncated recording plays to its end");
    check(cut_store.snapshot(rm_terminal::monotonic_now()).game.red_score.value == 9,
          "every complete record before the cut is applied");
    check(cut.statusText().contains(QStringLiteral("结尾不完整")),
          "status tells the operator the tail was cut");

    std::printf("replay_player_test: all checks passed\n");
    return 0;
}
