pragma Singleton
import QtQuick

// QML 侧的唯一配色/字号来源,数值与 cpp/ui/theme.h 逐一对应。「深空玻璃」:面板、行、按钮都是
// 半透明的,底下透出窗口背景的光晕。任何在两个面板里出现过的视觉常量都放这里,不要就地内联。
QtObject {
    readonly property color void_: "#070B14"
    readonly property color voidDeep: "#04060B"

    readonly property color surface: Qt.rgba(16 / 255, 26 / 255, 44 / 255, 0.60)
    readonly property color surfaceRaised: Qt.rgba(40 / 255, 58 / 255, 90 / 255, 0.38)
    readonly property color chip: Qt.rgba(12 / 255, 20 / 255, 34 / 255, 0.66)
    readonly property color row: Qt.rgba(30 / 255, 44 / 255, 70 / 255, 0.40)
    readonly property color rowSelf: Qt.rgba(44 / 255, 66 / 255, 104 / 255, 0.52)
    readonly property color tile: Qt.rgba(1, 1, 1, 0.035)
    readonly property color border: Qt.rgba(150 / 255, 180 / 255, 230 / 255, 0.16)
    readonly property color rowBorder: Qt.rgba(150 / 255, 180 / 255, 230 / 255, 0.10)
    readonly property color borderBright: Qt.rgba(160 / 255, 196 / 255, 1, 0.32)
    readonly property color highlight: Qt.rgba(1, 1, 1, 0.10)
    readonly property color overlay: Qt.rgba(10 / 255, 17 / 255, 30 / 255, 0.96)

    // 阵营色。刻意与告警红区分:「我是红方」和「这个值危险」不能看起来一样。
    readonly property color red: "#E43E4A"
    readonly property color redDim: "#601E26"
    readonly property color blue: "#2E86DE"
    readonly property color blueDim: "#183E68"
    readonly property color redText: "#FF8F96"
    readonly property color blueText: "#84C2FF"
    // 血条填充的 RGB(不含 alpha,用的地方各自给透明度)。
    readonly property color fillRed: "#F05260"
    readonly property color fillBlue: "#4098EE"
    readonly property color fillWarn: "#ECAA46"
    readonly property color fillDanger: "#FF5848"
    // 基地血条照 R 标取色,和 R 标是同一种红、蓝。
    readonly property color logoRed: "#D65352"
    readonly property color logoBlue: "#2377C0"
    readonly property color baseWarn: "#E29E36"
    readonly property color baseDanger: "#E8463A"

    readonly property color cyan: "#8ED2F2"
    readonly property color cyanDim: Qt.rgba(84 / 255, 156 / 255, 204 / 255, 0.32)
    readonly property color accentText: "#C4E9FB"
    readonly property color accentBg: Qt.rgba(18 / 255, 44 / 255, 66 / 255, 0.86)

    readonly property color textPrimary: "#E6EDF6"
    readonly property color textBright: "#C3CEDD"
    readonly property color textSecondary: "#93A3BA"
    readonly property color textMuted: "#6F7F98"

    readonly property color ok: "#3ECF8E"
    readonly property color okText: "#5FD39A"
    readonly property color warn: "#ECAA46"
    readonly property color danger: "#FF6464"
    readonly property color dangerText: "#FF9A9A"
    readonly property color offline: "#5B6474"

    // 数字用随程序打包的 Barlow Semi Condensed,配 font.features tnum 等宽;中文用系统黑体。
    // QML 的 font.family 只接受一个字族名,不能写逗号分隔的回退列表。
    readonly property string numericFamily: "Barlow Semi Condensed"
    readonly property string labelFamily: Qt.platform.os === "osx" ? "PingFang SC" : "Noto Sans CJK SC"
    readonly property string titleFamily: labelFamily
    readonly property var tabular: ({ "tnum": 1 })
    readonly property real titleSpacing: 2

    function factionColor(isBlue) { return isBlue ? blue : red }
    function factionDim(isBlue) { return isBlue ? blueDim : redDim }
    function factionText(isBlue) { return isBlue ? blueText : redText }
    function factionFill(isBlue) { return isBlue ? fillBlue : fillRed }
    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    // 兵种 → 头像素材。顶栏卡片和花名册行共用同一套 PNG,映射只能有一份。
    // 未知兵种返回空串 —— 猜错兵种比留空更糟。
    function avatarFor(robotClass, isBlue) {
        var base = "qrc:/images/top_robots/" + (isBlue ? "blue" : "red")
        if (robotClass.indexOf("哨兵") >= 0) return base + "_guard_avatar.png"
        if (robotClass.indexOf("英雄") >= 0 || robotClass.indexOf("重装") >= 0)
            return base + "_teammate_avatar_hero.png"
        if (robotClass.indexOf("工程") >= 0) return base + "_teammate_avatar_engineer.png"
        if (robotClass.indexOf("空中") >= 0) return base + "_teammate_avatar_airplane.png"
        if (robotClass.indexOf("步兵") >= 0) return base + "_teammate_avatar_soldier.png"
        return ""
    }

    function pedestalFor(isBlue) {
        return "qrc:/images/top_robots/" + (isBlue ? "blue" : "red") + "_teammate_bg.png"
    }

    // 血量决定颜色:一半以上用阵营色,一半以下橙黄,四分之一及以下红。与 theme.cpp 一致。
    function healthFill(ratio, isBlue) {
        if (ratio >= 0.5) return factionFill(isBlue)
        if (ratio > 0.25) return fillWarn
        return fillDanger
    }
    function healthColor(ratio) {
        if (ratio >= 0.5) return ok
        if (ratio > 0.25) return warn
        return danger
    }

    // level: 0 信息 1 提示 2 警告 3 严重。
    function eventColor(level) {
        if (level >= 3) return danger
        if (level === 2) return warn
        if (level === 1) return cyan
        return textSecondary
    }
}
