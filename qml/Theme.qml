pragma Singleton
import QtQuick

// QML 侧的唯一配色/字号来源,数值与 cpp/theme.h 逐一对应。
//
// 迁移前顶栏 QML 和 C++ 自绘面板各有一套颜色(顶栏红 #FF5555、C++ 红 rgb(228,62,74)),
// 同一个「红方」在两处不同色,这是上下割裂感的根源。任何在两个面板里出现过的视觉常量
// 都必须放这里,不要就地内联。
QtObject {
    // 表面色,由深到浅。
    readonly property color void_: "#07090D"
    readonly property color surface: "#0E121A"
    readonly property color surfaceRaised: "#141A25"
    readonly property color border: "#263042"
    readonly property color borderBright: "#3A4E6C"

    // 阵营色。刻意与告警红蓝区分:「我是红方」和「这个值危险」不能看起来一样。
    readonly property color red: "#E43E4A"
    readonly property color redDim: "#601E26"
    readonly property color blue: "#2E86DE"
    readonly property color blueDim: "#183E68"

    // 强调色,用于标题、激活边框。
    readonly property color cyan: "#40D0E8"
    readonly property color cyanDim: "#164E5C"

    // 文字,由亮到暗。
    readonly property color textPrimary: "#E2E8F0"
    readonly property color textSecondary: "#94A3B8"
    readonly property color textMuted: "#586476"

    // 状态语义。刻意压暗、降饱和:HUD 主体是深色,亮绿(#40D68A)/亮黄(#F0BA4A)那种
    // 高饱和荧光色铺在五行血条上会盖过真正需要抢注意的告警,整屏显得吵。
    // 现在的取值仍能拉开绿/黄/红三档语义,但亮度退到背景层级之上一档即止。
    readonly property color ok: "#2E9E68"
    readonly property color warn: "#B8893A"
    readonly property color danger: "#C84A4A"
    readonly property color offline: "#3C4350"

    // 字体分三层,各有明确职责:
    //
    // numericFamily —— 仪表数字。原本用 Menlo,那是终端等宽体、字形棱角极硬,
    //   和中文黑体混排时锋利得格格不入。DIN Alternate 是工程仪表标准字,
    //   字宽仍然齐整(数值跳动不会导致整行重排),但字形圆润,和中文能协调。
    // labelFamily —— 中文正文/数据标签。
    // titleFamily —— 面板标题。标题要跳出正文,靠「字族 + 字距 + 字号」三者拉开,
    //   不是单纯调大字号;Avenir Next 的几何感配 PingFang 的中文标题很稳。
    readonly property string numericFamily: "DIN Alternate"
    readonly property string labelFamily: "PingFang SC"
    // 单个字族名 —— QML 的 font.family 不接受逗号分隔的回退列表,
    // 写 "A, B" 会被当成一个不存在的字族名并触发全表别名扫描。
    // PingFang SC 同时覆盖中英文,标题的区分交给字号和字距。
    readonly property string titleFamily: "PingFang SC"

    // 标题字距。中文标题拉开字距会显著提升「这是标题」的识别度,
    // 这是各家比赛 UI 里标题最通用的处理。
    readonly property real titleSpacing: 2.4

    function factionColor(isBlue) { return isBlue ? blue : red }
    function factionDim(isBlue) { return isBlue ? blueDim : redDim }

    // 兵种 → 头像素材。顶栏卡片和花名册行共用同一套 PNG,映射只能有一份:
    // 两处各写一份,以后改了类别名就会一边有图、一边空白。
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



    // 血量决定色相:60% 以上绿、25% 以上琥珀、以下红。操作手先读颜色再读数字,
    // 所以颜色必须承载同样的警示等级。与 theme.cpp 的 healthColor 保持一致。
    function healthColor(ratio) {
        if (ratio > 0.6) return ok
        if (ratio > 0.25) return warn
        return danger
    }

    // level: 0 信息 1 提示 2 警告 3 严重。官方选手端语义:中立白、有利绿、不利红。
    function eventColor(level) {
        if (level >= 3) return danger
        if (level === 2) return warn
        if (level === 1) return cyan
        return textSecondary
    }
}
