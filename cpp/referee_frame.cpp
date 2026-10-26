#include "referee_frame.h"

#include <array>

namespace rm_terminal {
namespace {

// 数据段上限。帧头 CRC8 只有 1/256 的误判率,垃圾字节偶尔会凑出一个「合法」帧头;
// 不设上限的话,一个读出 60000 的假长度会让解析器干等 5 秒(115200 波特率),这段时间
// 里真正的好帧全堵在缓冲里。控制器只发 30 字节,256 足够留余量。
constexpr std::uint16_t kMaxDataBytes = 256;
constexpr qsizetype kHeaderBytes = 5;
constexpr qsizetype kOverheadBytes = kHeaderBytes + 2 + 2;

constexpr std::array<std::uint8_t, 256> make_crc8_table() {
    std::array<std::uint8_t, 256> table{};
    for (int index = 0; index < 256; ++index) {
        std::uint8_t crc = static_cast<std::uint8_t>(index);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1) ? static_cast<std::uint8_t>((crc >> 1) ^ 0x8C)
                            : static_cast<std::uint8_t>(crc >> 1);
        table[static_cast<std::size_t>(index)] = crc;
    }
    return table;
}

constexpr std::array<std::uint16_t, 256> make_crc16_table() {
    std::array<std::uint16_t, 256> table{};
    for (int index = 0; index < 256; ++index) {
        std::uint16_t crc = static_cast<std::uint16_t>(index);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1) ? static_cast<std::uint16_t>((crc >> 1) ^ 0x8408)
                            : static_cast<std::uint16_t>(crc >> 1);
        table[static_cast<std::size_t>(index)] = crc;
    }
    return table;
}

constexpr auto kCrc8Table = make_crc8_table();
constexpr auto kCrc16Table = make_crc16_table();

std::uint16_t read_u16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0] | (data[1] << 8));
}

void append_u16(QByteArray& out, std::uint16_t value) {
    out.append(static_cast<char>(value & 0xff));
    out.append(static_cast<char>((value >> 8) & 0xff));
}

}  // namespace

std::uint8_t referee_crc8(const std::uint8_t* data, std::size_t size) {
    std::uint8_t crc = 0xFF;
    for (std::size_t index = 0; index < size; ++index) crc = kCrc8Table[crc ^ data[index]];
    return crc;
}

std::uint16_t referee_crc16(const std::uint8_t* data, std::size_t size) {
    std::uint16_t crc = 0xFFFF;
    for (std::size_t index = 0; index < size; ++index)
        crc = static_cast<std::uint16_t>((crc >> 8) ^ kCrc16Table[(crc ^ data[index]) & 0xff]);
    return crc;
}

QByteArray encode_referee_frame(std::uint16_t cmd_id, const QByteArray& data, std::uint8_t seq) {
    QByteArray frame;
    frame.reserve(kOverheadBytes + data.size());
    frame.append(static_cast<char>(kRefereeSof));
    append_u16(frame, static_cast<std::uint16_t>(data.size()));
    frame.append(static_cast<char>(seq));
    frame.append(static_cast<char>(
        referee_crc8(reinterpret_cast<const std::uint8_t*>(frame.constData()), 4)));
    append_u16(frame, cmd_id);
    frame.append(data);
    append_u16(frame, referee_crc16(reinterpret_cast<const std::uint8_t*>(frame.constData()),
                                    static_cast<std::size_t>(frame.size())));
    return frame;
}

void RefereeFrameParser::feed(const char* bytes, qsizetype size) {
    buffer_.append(bytes, size);
}

std::optional<RefereeFrame> RefereeFrameParser::next() {
    while (true) {
        const qsizetype start = buffer_.indexOf(static_cast<char>(kRefereeSof));
        if (start < 0) {
            buffer_.clear();
            return std::nullopt;
        }
        if (start > 0) buffer_.remove(0, start);
        if (buffer_.size() < kHeaderBytes) return std::nullopt;

        const auto* bytes = reinterpret_cast<const std::uint8_t*>(buffer_.constData());
        const std::uint16_t length = read_u16(bytes + 1);
        if (referee_crc8(bytes, 4) != bytes[4] || length > kMaxDataBytes) {
            ++bad_frames_;
            buffer_.remove(0, 1);
            continue;
        }

        const qsizetype total = kOverheadBytes + length;
        if (buffer_.size() < total) return std::nullopt;
        if (referee_crc16(bytes, static_cast<std::size_t>(total - 2)) !=
            read_u16(bytes + total - 2)) {
            ++bad_frames_;
            buffer_.remove(0, 1);
            continue;
        }

        RefereeFrame frame;
        frame.seq = bytes[3];
        frame.cmd_id = read_u16(bytes + kHeaderBytes);
        frame.data = buffer_.mid(kHeaderBytes + 2, length);
        buffer_.remove(0, total);
        return frame;
    }
}

}  // namespace rm_terminal
