#pragma once
#include "store.h"
#include <QObject>
#include <QString>

namespace rm_terminal {
class MqttIntake final : public QObject {
    Q_OBJECT
public:
    MqttIntake(Store& store, QString host, int port, QObject* parent = nullptr);
    ~MqttIntake() override;
    bool start();
    bool readonly() const { return true; }

    // 链路当前是否连着。mosquitto 的回调跑在它自己的网络线程上,所以底下存的是
    // atomic:GUI 的定时 tick 直接读它,不必等一次信号投递。战术提醒靠它区分
    // 「比赛真的暂停了」和「我们已经看不见比赛了」——后者必须停止播报。
    bool connected() const;

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
