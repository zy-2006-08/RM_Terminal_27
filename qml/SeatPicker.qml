import QtQuick
import "."

// 开机选席位页。两步:先阵营,再席位,两步都不预选 —— 席位决定整场的布局和 MQTT
// 登录名,猜错比多点一下代价大得多。
//
// 合法席位、编号换算和摘要文字全部由 C++ 侧(Seat)算好后灌进来:同一份「哪些席位
// 能选」的规则不能在 QML 再写一份,否则规则手册改了只会有一边跟着改。本文件只负责
// 「点了什么」和「现在选到哪一步」的呈现。
Item {
    id: root

    // C++ 灌入。seats 的每项是 {number, title, cls}:number 是红方编号(蓝方由 C++ 加 100),
    // title 是界面上的兵种名,cls 喂给 Theme.avatarFor 取头像(指挥手走「空中」那张)。
    property var seats: []
    // 0 = 还没选。阵营 1 红 2 蓝。
    property int factionChoice: 0
    property int seatChoice: 0
    // 两步都选完后 C++ 给出的整句摘要;没选完是空串,整条不显示。
    property string summaryText: ""
    property bool confirmEnabled: false

    // 用户动作只上报「点了什么」,不自行推导结果:确认是否可用、编号怎么换算都在 C++。
    signal factionRequested(int faction)
    signal seatRequested(int number)
    signal confirmRequested()
    signal cancelRequested()

    readonly property bool factionKnown: factionChoice !== 0
    readonly property bool isBlue: factionChoice === 2
    // 阵营未定时用青色表达「这一项已选中」:此时染红或染蓝都是在替用户做决定。
    readonly property color accentColor: factionKnown ? Theme.factionColor(isBlue) : Theme.cyan
    readonly property color accentDim: factionKnown ? Theme.factionDim(isBlue) : Theme.cyanDim

    // 尺寸按 1280x760 的设计稿等比缩放,下限 0.72 —— 程序最小窗口是 960x600,
    // 不夹住下限会把按钮缩到坐在座位上点不中。
    readonly property real k: Math.max(0.72, Math.min(1.0,
        Math.min(width / 1280, height / 760)))

    // 步骤标题:序号徽标 + 名称 + 当前选择。徽标在选过之后由空心变实心,
    // 让「还差哪一步」不必靠读文字。
    //
    // 内联组件不与外层共享作用域,所以缩放系数和强调色都显式传进来,不读 root。
    component StepHeader: Item {
        id: header
        property string step: ""
        property string label: ""
        property string hint: ""
        property bool done: false
        property real ratio: 1.0
        property color accentColor: Theme.cyan

        height: Math.round(26 * header.ratio)

        Rectangle {
            id: badge
            width: Math.round(22 * header.ratio)
            height: width
            radius: width / 2
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            color: header.done ? header.accentColor : "transparent"
            border.width: 1
            border.color: header.done ? header.accentColor : Theme.borderBright

            Behavior on color { ColorAnimation { duration: 120 } }

            Text {
                anchors.centerIn: parent
                text: header.step
                color: header.done ? Theme.void_ : Theme.textSecondary
                font.family: Theme.numericFamily
                font.pixelSize: Math.round(13 * header.ratio)
                font.bold: true
            }
        }

        Text {
            id: labelText
            anchors.left: badge.right
            anchors.leftMargin: Math.round(9 * header.ratio)
            anchors.verticalCenter: parent.verticalCenter
            text: header.label
            color: Theme.textPrimary
            font.family: Theme.titleFamily
            font.pixelSize: Math.round(15 * header.ratio)
            font.bold: true
            font.letterSpacing: Theme.titleSpacing
        }

        Text {
            anchors.left: labelText.right
            anchors.leftMargin: Math.round(12 * header.ratio)
            anchors.baseline: labelText.baseline
            text: header.hint
            color: header.done ? header.accentColor : Theme.textMuted
            font.family: Theme.labelFamily
            font.pixelSize: Math.round(13 * header.ratio)
        }
    }

    Panel {
        id: shell
        anchors.fill: parent
        anchors.margins: Math.round(32 * root.k)
        title: "选择席位"
        accent: Theme.cyan

        // 四列网格:席位卡占满四列,阵营按钮占前两列。两行左边界对齐,
        // 整页只有一条纵向基准线。
        readonly property real gap: Math.round(18 * root.k)
        readonly property real columnWidth: Math.max(1, (width - 12 - 3 * gap) / 4)

        Column {
            id: content
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: Math.round(22 * root.k)

            StepHeader {
                width: content.width
                step: "1"
                label: "阵营"
                done: root.factionKnown
                ratio: root.k
                accentColor: root.accentColor
                hint: root.factionKnown
                    ? (root.isBlue ? "已选 蓝方" : "已选 红方")
                    : "未选"
            }

            Row {
                spacing: shell.gap

                Repeater {
                    model: [{ code: 1, name: "红方", blue: false },
                            { code: 2, name: "蓝方", blue: true }]

                    Rectangle {
                        id: factionButton
                        required property var modelData
                        readonly property bool chosen: root.factionChoice === factionButton.modelData.code
                        readonly property color tint: Theme.factionColor(factionButton.modelData.blue)

                        width: shell.columnWidth
                        height: Math.round(72 * root.k)
                        radius: 3
                        // 选中:阵营色实底 + 亮边;未选:近乎卡面的暗底 + 暗阵营色边。
                        // 两态的差异同时落在底色和边框上,2 米外先看到的是色块而不是边线。
                        color: factionButton.chosen
                            ? Theme.factionDim(factionButton.modelData.blue)
                            : (factionMouse.containsMouse ? Theme.surfaceRaised : Theme.surface)
                        border.width: factionButton.chosen ? 3 : 1
                        border.color: factionButton.chosen
                            ? factionButton.tint
                            : (factionMouse.containsMouse
                                ? Qt.lighter(factionButton.tint, 1.15)
                                : Qt.darker(factionButton.tint, 2.0))

                        Behavior on color { ColorAnimation { duration: 120 } }
                        Behavior on border.color { ColorAnimation { duration: 120 } }

                        Text {
                            anchors.centerIn: parent
                            text: factionButton.modelData.name
                            color: factionButton.chosen
                                ? Theme.textPrimary
                                : Qt.lighter(factionButton.tint, 1.1)
                            font.family: Theme.titleFamily
                            font.pixelSize: Math.round(24 * root.k)
                            font.bold: true
                            font.letterSpacing: Theme.titleSpacing
                        }

                        MouseArea {
                            id: factionMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.factionRequested(factionButton.modelData.code)
                        }
                    }
                }
            }

            StepHeader {
                width: content.width
                step: "2"
                label: "席位"
                done: root.seatChoice !== 0
                ratio: root.k
                accentColor: root.accentColor
                hint: root.seatChoice !== 0 ? "已选 " + root.seatChoice + " 号" : "未选"
            }

            Row {
                spacing: shell.gap

                Repeater {
                    model: root.seats

                    Rectangle {
                        id: seatCard
                        required property var modelData
                        readonly property bool chosen: root.seatChoice === seatCard.modelData.number

                        width: shell.columnWidth
                        height: Math.round(202 * root.k)
                        radius: 3
                        color: (seatCard.chosen || seatMouse.containsMouse)
                            ? Theme.surfaceRaised
                            : Theme.surface
                        border.width: seatCard.chosen ? 3 : 1
                        border.color: seatCard.chosen
                            ? root.accentColor
                            : (seatMouse.containsMouse ? Theme.borderBright : Theme.border)

                        Behavior on color { ColorAnimation { duration: 120 } }
                        Behavior on border.color { ColorAnimation { duration: 120 } }

                        // 选中卡的「抬起」感:阵营色内发光一圈。纯换边框在深色底上
                        // 远看只是一条细线,加一层暗阵营色底光才能隔着两米认出来。
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 3
                            radius: 2
                            color: root.accentDim
                            opacity: seatCard.chosen ? 0.45 : 0
                            Behavior on opacity { NumberAnimation { duration: 120 } }
                        }

                        // 花名册同款头像卡,关掉卡内血条和角标编号 —— 这里没有血量,
                        // 编号也已经由下面的标题写全。
                        RobotCard {
                            id: portrait
                            width: Math.round(seatCard.width * 0.44)
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: parent.top
                            anchors.topMargin: Math.round(14 * root.k)
                            robotId: seatCard.modelData.number
                            isBlue: root.isBlue
                            robotClass: seatCard.modelData.cls
                            hasHp: false
                            // 阵营没定之前头像压暗:此时画成红方的满亮头像等于替用户
                            // 预选了红方。online 这一路正好是「不在线就压暗」的现成画法。
                            online: root.factionKnown
                            showBar: false
                            showId: false
                        }

                        Text {
                            id: seatTitle
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: portrait.bottom
                            anchors.topMargin: Math.round(8 * root.k)
                            text: seatCard.modelData.number + " 号"
                            color: seatCard.chosen ? Theme.textPrimary : Theme.textSecondary
                            font.family: Theme.numericFamily
                            font.pixelSize: Math.round(22 * root.k)
                            font.bold: true
                        }

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: seatTitle.bottom
                            anchors.topMargin: Math.round(2 * root.k)
                            text: seatCard.modelData.title
                            color: seatCard.chosen ? root.accentColor : Theme.textMuted
                            font.family: Theme.labelFamily
                            font.pixelSize: Math.round(15 * root.k)
                            font.bold: true
                            font.letterSpacing: 1.2
                        }

                        MouseArea {
                            id: seatMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.seatRequested(seatCard.modelData.number)
                        }
                    }
                }
            }

            // 摘要条。两步都选完才出现:没选完时留一条空壳只会让人以为还有东西没加载。
            // 占位高度保持不变,出现和消失不会把下面的按钮抬上抬下。
            Item {
                width: content.width
                height: Math.round(60 * root.k)

                Rectangle {
                    anchors.fill: parent
                    radius: 3
                    visible: root.summaryText !== ""
                    color: Theme.surfaceRaised
                    border.width: 1
                    border.color: root.accentColor

                    Rectangle {
                        width: 3
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.margins: 1
                        color: root.accentColor
                    }

                    Text {
                        objectName: "summaryLine"
                        anchors.left: parent.left
                        anchors.leftMargin: Math.round(16 * root.k)
                        anchors.right: parent.right
                        anchors.rightMargin: Math.round(16 * root.k)
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.summaryText
                        color: Theme.textPrimary
                        font.family: Theme.labelFamily
                        font.pixelSize: Math.round(16 * root.k)
                        font.bold: true
                        font.letterSpacing: 0.6
                        elide: Text.ElideRight
                    }
                }

                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.summaryText === ""
                    text: "两步都选过之后才能进入"
                    color: Theme.textMuted
                    font.family: Theme.labelFamily
                    font.pixelSize: Math.round(13 * root.k)
                }
            }

            Text {
                text: "本次运行不能更换席位，换席位要重启终端"
                color: Theme.textMuted
                font.family: Theme.labelFamily
                font.pixelSize: Math.round(13 * root.k)
            }

            Row {
                spacing: shell.gap

                Rectangle {
                    id: confirmButton
                    width: shell.columnWidth
                    height: Math.round(54 * root.k)
                    radius: 3
                    // 不可用时整块退到卡面色 + 暗边 + 暗字:一眼就是「现在点不了」,
                    // 而不是「点了没反应」。
                    color: !root.confirmEnabled
                        ? Theme.surface
                        : (confirmMouse.containsMouse ? Qt.lighter(Theme.cyanDim, 1.3) : Theme.cyanDim)
                    border.width: root.confirmEnabled ? 2 : 1
                    border.color: root.confirmEnabled ? Theme.cyan : Theme.border

                    Behavior on color { ColorAnimation { duration: 120 } }

                    Text {
                        anchors.centerIn: parent
                        text: "确认并进入"
                        color: root.confirmEnabled ? Theme.textPrimary : Theme.textMuted
                        font.family: Theme.titleFamily
                        font.pixelSize: Math.round(16 * root.k)
                        font.bold: true
                        font.letterSpacing: Theme.titleSpacing
                    }

                    MouseArea {
                        id: confirmMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: root.confirmEnabled
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.confirmRequested()
                    }
                }

                Rectangle {
                    id: cancelButton
                    width: Math.round(shell.columnWidth * 0.62)
                    height: Math.round(54 * root.k)
                    radius: 3
                    color: cancelMouse.containsMouse ? Theme.surfaceRaised : Theme.surface
                    border.width: 1
                    border.color: cancelMouse.containsMouse ? Theme.borderBright : Theme.border

                    Behavior on color { ColorAnimation { duration: 120 } }

                    Text {
                        anchors.centerIn: parent
                        text: "退出"
                        color: Theme.textSecondary
                        font.family: Theme.titleFamily
                        font.pixelSize: Math.round(16 * root.k)
                        font.bold: true
                        font.letterSpacing: Theme.titleSpacing
                    }

                    MouseArea {
                        id: cancelMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.cancelRequested()
                    }
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Enter 确认 · Esc 退出"
                    color: Theme.textMuted
                    font.family: Theme.labelFamily
                    font.pixelSize: Math.round(13 * root.k)
                }
            }
        }
    }
}
