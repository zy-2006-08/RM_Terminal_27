#pragma once
#include "store.h"
#include <QByteArray>
#include <QObject>
#include <QString>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace rm_terminal {
class MqttIntake final : public QObject {
    Q_OBJECT
public:
    MqttIntake(Store& store, QString host, int port, int self_robot_id,
               QObject* parent = nullptr);
    ~MqttIntake() override;
    bool start();

    // 链路当前是否连着。mosquitto 的回调跑在它自己的网络线程上,所以底下存的是
    // atomic:GUI 的定时 tick 直接读它,不必等一次信号投递。战术提醒靠它区分
    // 「比赛真的暂停了」和「我们已经看不见比赛了」——后者必须停止播报。
    bool connected() const;

    // 链路状态灯要的原始量。只看 connected 不够:网线被拔时连接要等保活超时才断,
    // 在那之前「连着但没数据」只能靠最后一条消息的时间看出来。
    struct LinkStats {
        bool connected = false;
        std::uint64_t messages = 0;
        std::optional<MonotonicMs> last_message_at;
        std::optional<MonotonicMs> connected_at;
    };
    LinkStats linkStats() const;

    // 必须在 start() 之前设置。
    void setKeepAlive(int seconds);

    // 向服务器发一条消息。没连上时返回 false 且什么都不排队。
    bool publish(const char* topic, const std::string& payload);

    // 录制钩子:解码前拿到原始 topic/payload。回调跑在 mosquitto 网络线程上,
    // 因此必须线程安全(Recorder 自带互斥)。必须在 start() 之前设置。
    void setMessageObserver(
        std::function<void(const QString&, const QByteArray&)> observer);

Q_SIGNALS:
    void diagnostic(QString text);
    // 连接状态变化。从网络线程发出,Qt 的自动连接会把它排到接收者所在的线程,
    // 因此槽函数里可以安全地碰界面。
    void connectionChanged(bool connected);
public:
    struct Impl;
    Impl* impl_;
};
}
