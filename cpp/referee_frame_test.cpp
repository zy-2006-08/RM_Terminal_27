#include "referee_frame.h"

#include <QByteArray>

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void check(bool condition, const char* label) {
    if (condition) return;
    std::fprintf(stderr, "FAIL %s\n", label);
    ++failures;
}

QByteArray joints_payload(const float (&joints)[6]) {
    QByteArray data(rm_terminal::kCustomControllerDataBytes, '\0');
    std::memcpy(data.data(), joints, sizeof(joints));
    return data;
}

// 控制器固件(RM_Lib.hpp Data_Concatenation + 两张 CRC 表)按同样输入打出来的帧。
// 字节由固件的查表逐字节复算得到;任何一张表抄错一项,这里就对不上。
const char kFirmwareFrameHex[] =
    "a51e0007fe0203cdcccc3dcdcc4c3e9a99993ecdcccc3e0000003f9a99193f0000000000004dee";

void crc_matches_the_firmware_tables() {
    const auto* text = reinterpret_cast<const std::uint8_t*>("123456789");
    // CRC-16/MCRF4XX 的标准校验值:多项式 0x8408、初值 0xFFFF、不取反。
    check(rm_terminal::referee_crc16(text, 9) == 0x6F91, "crc16 standard check value");
    check(rm_terminal::referee_crc8(text, 9) == 0x0B, "crc8 with init 0xFF");
}

void encoding_matches_a_firmware_frame() {
    const float joints[6] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
    const QByteArray frame = rm_terminal::encode_referee_frame(
        rm_terminal::kCustomControllerCmd, joints_payload(joints), 7);
    check(frame.size() == 39, "a 30-byte payload makes a 39-byte frame");
    check(frame.toHex() == QByteArray(kFirmwareFrameHex), "byte-identical to the firmware frame");
}

void a_frame_split_across_reads_still_parses() {
    const QByteArray frame = QByteArray::fromHex(kFirmwareFrameHex);
    rm_terminal::RefereeFrameParser parser;
    parser.feed(frame.constData(), 4);
    check(!parser.next().has_value(), "half a header is not a frame yet");
    parser.feed(frame.constData() + 4, 20);
    check(!parser.next().has_value(), "half a frame is not a frame yet");
    parser.feed(frame.constData() + 24, frame.size() - 24);
    const auto parsed = parser.next();
    check(parsed.has_value(), "the completed frame parses");
    if (!parsed) return;
    check(parsed->cmd_id == rm_terminal::kCustomControllerCmd, "command id 0x0302");
    check(parsed->seq == 7, "sequence number");
    check(parsed->data.size() == rm_terminal::kCustomControllerDataBytes, "30 data bytes");
    float joints[6] = {};
    std::memcpy(joints, parsed->data.constData(), sizeof(joints));
    check(joints[0] == 0.1f && joints[5] == 0.6f, "joint floats survive little-endian");
    check(parser.badFrames() == 0, "a clean stream has no bad frames");
}

void garbage_and_a_corrupt_frame_do_not_hide_the_next_good_one() {
    const QByteArray good = QByteArray::fromHex(kFirmwareFrameHex);
    QByteArray corrupt = good;
    corrupt[20] = static_cast<char>(corrupt[20] ^ 0x01);

    // 插拔时的典型字节流:一段含 0xA5 的垃圾、一帧数据段被翻转一位的坏帧、两帧好帧。
    QByteArray stream = QByteArray::fromHex("00a5ff13a5");
    stream += corrupt;
    stream += good;
    stream += good;

    rm_terminal::RefereeFrameParser parser;
    parser.feed(stream.constData(), stream.size());
    int parsed = 0;
    while (const auto frame = parser.next()) {
        check(frame->data == good.mid(7, 30), "only the intact payload comes out");
        ++parsed;
    }
    check(parsed == 2, "both good frames survive the garbage and the corrupt frame");
    check(parser.badFrames() >= 1, "the corrupt frame is counted");
}

void an_absurd_length_resynchronises_immediately() {
    // 帧头 CRC8 正确但长度 0xFFFF:不能为它干等 65535 字节。
    QByteArray fake = QByteArray::fromHex("a5ffff00");
    fake.append(static_cast<char>(
        rm_terminal::referee_crc8(reinterpret_cast<const std::uint8_t*>(fake.constData()), 4)));
    const QByteArray good = QByteArray::fromHex(kFirmwareFrameHex);

    rm_terminal::RefereeFrameParser parser;
    parser.feed(fake.constData(), fake.size());
    parser.feed(good.constData(), good.size());
    const auto frame = parser.next();
    check(frame.has_value(), "the good frame after a fake length header parses at once");
    check(parser.badFrames() == 1, "the fake header is counted once");
}

}  // namespace

int main() {
    crc_matches_the_firmware_tables();
    encoding_matches_a_firmware_frame();
    a_frame_split_across_reads_still_parses();
    garbage_and_a_corrupt_frame_do_not_hide_the_next_good_one();
    an_absurd_length_resynchronises_immediately();
    if (failures != 0) {
        std::fprintf(stderr, "referee_frame_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("referee_frame_test: all checks passed\n");
    return 0;
}
