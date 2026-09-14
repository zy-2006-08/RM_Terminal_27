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
Q_SIGNALS:
    void diagnostic(QString text);
public:
    struct Impl;
    Impl* impl_;
};
}
