#pragma once

#include <QString>

namespace rm_terminal
{

    // P 设置面板里终端自己的几项。和官方选手端一样按这台电脑存,不跟着操作手和方案走。
    struct TerminalSettings
    {
        int sensitivity = 50;        // 控制灵敏度 1–100:键鼠控制时鼠标移动乘的倍数,50 = 原样
        int volume = 100;            // 提醒和语音的音量 0–100
        bool video_reversed = false; // 图传方向「反向」:画面转 180°,相机倒着装时用
    };

    bool operator==(const TerminalSettings& left, const TerminalSettings& right);
    bool operator!=(const TerminalSettings& left, const TerminalSettings& right);

    // 50 → 1.0 倍,100 → 2.0 倍,1 → 0.02 倍。
    double sensitivity_scale(int sensitivity);
    // 超出范围的值拉回范围里(文件被手改坏了也能用)。
    TerminalSettings normalized(TerminalSettings settings);

    QString terminal_settings_path();
    // 文件不存在时是默认值;读不懂时也回默认值,error 写原因。
    TerminalSettings load_terminal_settings(const QString& path, QString* error = nullptr);
    bool save_terminal_settings(const QString& path, const TerminalSettings& settings, QString* error = nullptr);

} // namespace rm_terminal
