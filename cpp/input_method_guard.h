#pragma once

#include <QObject>
#include <QSet>
#include <QString>

namespace rm_terminal {

// 中文输入法只给真正的打字框。复活和兑换的 Y/N、H、O、I、P,键鼠控制的 W/A/S/D 都是字母:
// 焦点落在一个对输入法说「我能接字」的东西上时,输入法(ibus/Rime)会先把字母拿去拼字,
// 终端就收不到这个键(协议上什么都没发)。装在 QApplication 上,对不是打字框的对象一律答「不接」。
class InputMethodGuard final : public QObject {
    Q_OBJECT
public:
    explicit InputMethodGuard(QObject* parent = nullptr);

    // 看得见的单行/多行输入框、数字框、可编辑下拉框,以及 QML 里自己声明接输入法的文字框。
    static bool takesText(const QObject* object);

Q_SIGNALS:
    // 拦下了一个本来要接输入法的东西(每种只报一次),写进日志好查是哪一块。
    void blocked(const QString& what);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QSet<QString> reported_;
};

}  // namespace rm_terminal
