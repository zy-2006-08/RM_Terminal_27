#pragma once

#include <QByteArray>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace rm_terminal {

// 裁判系统串口帧。自定义控制器沿用同一格式:
//   0xA5 | u16 data_length | u8 seq | u8 CRC8 | u16 cmd_id | data | u16 CRC16
// 整数小端。CRC8 覆盖前 4 字节,CRC16 覆盖它之前的整帧。
// CRC8 是 Maxim 反射多项式 0x8C、初值 0xFF;CRC16 是反射多项式 0x8408、初值 0xFFFF。
// 两张表与控制器固件 RM_Lib.hpp 里的查表逐项一致(referee_frame_test 用固件格式的
// 样帧锁住)。
constexpr std::uint8_t kRefereeSof = 0xA5;
constexpr std::uint16_t kCustomControllerCmd = 0x0302;
constexpr int kCustomControllerDataBytes = 30;

// 车回传给控制器的机械臂姿态:6 个 float 关节角 + 1 字节标志位(2 = 让控制器跟随车)。
// 2027 协议取消了 0x0309,车改用 0x0310(MQTT 上的 CustomByteBlock)发到终端,终端再按
// 控制器固件认的 0x0309 帧格式写给控制器,固件不用改。
constexpr std::uint16_t kArmFeedbackCmd = 0x0309;
constexpr int kArmFeedbackBytes = 25;

std::uint8_t referee_crc8(const std::uint8_t* data, std::size_t size);
std::uint16_t referee_crc16(const std::uint8_t* data, std::size_t size);

QByteArray encode_referee_frame(std::uint16_t cmd_id, const QByteArray& data, std::uint8_t seq);

// CustomByteBlock 的数据 -> 发给控制器的 0x0309 帧(数据段补零到 30 字节,和以前车发的一样)。
// 不足 25 字节或关节角不是有限数时返回空:这些字节来自网络,不能原样喂给机械臂。
std::optional<QByteArray> arm_feedback_frame(const QByteArray& custom_bytes, std::uint8_t seq);

struct RefereeFrame {
    std::uint16_t cmd_id = 0;
    std::uint8_t seq = 0;
    QByteArray data;
};

// 串口字节流 -> 帧。串口没有边界:一次 read 可能是半帧,也可能是好几帧,插拔时还会
// 混进垃圾字节。校验失败只丢掉那一个 0xA5 再往后找,不整段清空 —— 清空会把紧跟在
// 坏帧后面的好帧一起扔掉。
class RefereeFrameParser {
public:
    void feed(const char* bytes, qsizetype size);
    std::optional<RefereeFrame> next();
    // 只清缓冲,不清坏帧计数:重开串口后计数要接着累计,否则一次插拔就把证据抹掉。
    void clearBuffer() { buffer_.clear(); }
    std::uint64_t badFrames() const { return bad_frames_; }

private:
    QByteArray buffer_;
    std::uint64_t bad_frames_ = 0;
};

}  // namespace rm_terminal
