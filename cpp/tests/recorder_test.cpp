#include "recorder.h"

#include <QFile>
#include <QTemporaryDir>

#include <cstdio>
#include <stdexcept>

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() {
    QTemporaryDir directory;
    if (!directory.isValid()) throw std::runtime_error("temporary directory unavailable");

    rm_terminal::RecordingMetadata metadata;
    metadata.self_robot_id = 3;
    metadata.mqtt_host = QStringLiteral("127.0.0.1");
    metadata.mqtt_port = 3333;
    metadata.udp_port = 3334;

    rm_terminal::Recorder recorder;
    QString error;
    check(recorder.start(directory.path(), metadata, &error),
          "recorder starts in an empty directory");
    check(recorder.active(), "recorder reports active");
    check(recorder.statusText().contains(QStringLiteral("REC")),
          "active status is visible as REC");
    check(recorder.path().endsWith(QStringLiteral(".rmrec")),
          "recording uses the documented extension");

    const QByteArray game_status = QByteArray::fromHex("08011200ff");
    const QByteArray video = QByteArray::fromHex("0001000200000005aabbccdd");
    recorder.recordMqtt(QStringLiteral("GameStatus"), game_status);
    recorder.recordVideo(video);
    recorder.recordMqtt(QStringLiteral("Event"), QByteArray("hello"));
    check(recorder.records() == 3, "three records were accepted");

    const QString path = recorder.path();
    const std::uint64_t bytes_before_stop = recorder.bytes();
    check(bytes_before_stop > 3 * 15, "size counts headers and payloads");
    recorder.stop();
    check(!recorder.active(), "recorder stops");
    check(QFile::exists(path), "recording file exists");

    rm_terminal::RecordingReader reader;
    check(reader.open(path, &error), "reader opens the file");
    check(reader.metadata().value(QStringLiteral("self_robot_id")).toInt() == 3,
          "metadata carries the robot id");
    check(reader.metadata().value(QStringLiteral("format")).toString() ==
              QStringLiteral("rmrec"),
          "metadata names the format");

    rm_terminal::RecordingRecord record;
    check(reader.next(&record, &error), "first record reads");
    check(record.kind == rm_terminal::RecordingRecord::Kind::Mqtt,
          "first record is MQTT");
    check(record.topic == QStringLiteral("GameStatus"), "topic survives the round trip");
    check(record.payload == game_status, "binary payload survives the round trip");

    check(reader.next(&record, &error), "second record reads");
    check(record.kind == rm_terminal::RecordingRecord::Kind::Video,
          "second record is video");
    check(record.topic.isEmpty(), "video records carry no topic");
    check(record.payload == video, "raw datagram survives the round trip");

    check(reader.next(&record, &error), "third record reads");
    check(record.topic == QStringLiteral("Event"), "third topic reads");
    check(record.payload == QByteArray("hello"), "third payload reads");
    check(!reader.next(&record, &error), "reader reports end of file");
    check(reader.atEnd() && reader.error().isEmpty(), "clean EOF is not an error");

    // 强杀/断电时会留下半条记录。回放必须停在那里并报错,不能把半条 payload
    // 当完整消息喂给解码器。
    QFile truncated(path);
    check(truncated.open(QIODevice::Append), "can append a partial record");
    truncated.write("12345", 5);
    truncated.close();

    rm_terminal::RecordingReader damaged;
    check(damaged.open(path, &error), "damaged file still opens (metadata intact)");
    for (int index = 0; index < 3; ++index)
        check(damaged.next(&record, &error), "valid records before truncation still read");
    check(!damaged.next(&record, &error), "truncated record is rejected");
    check(damaged.error().contains(QStringLiteral("truncated")),
          "truncation names the failure");

    std::printf("recorder_test: all checks passed\n");
    return 0;
}
