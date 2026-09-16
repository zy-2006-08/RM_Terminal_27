import QtQuick
import "."

// 花名册单行。视觉主体直接复用顶栏的 RobotCard(立体底板 + 兵种渲染图素材),
// 而不是另画一套图标:同一个「3 号步兵」在顶栏和花名册必须长得一样,
// 否则操作手要在两套视觉语言之间做翻译。
//
// 三态(Alive/Kia/NoData)必须视觉可分:「阵亡」是可利用的战机,「无数据」是情报缺失,
// 画成同一种灰会让操作手做出相反决策。
Item {
    id: root

    property int robotId: 0
    property bool isBlue: false
    property int liveState: 0        // 0=NoData 1=Alive 2=Kia
    property bool hasHp: false
    property int currentHp: 0
    property int maxHp: 0
    property string robotClass: ""
    property bool hasPosition: false
    property bool positionStale: false

    readonly property bool kia: liveState === 2
    readonly property bool noData: liveState === 0
    readonly property bool online: liveState !== 0
    readonly property int shownId: robotId > 100 ? robotId - 100 : robotId
    readonly property real ratio: (hasHp && maxHp > 0)
        ? Math.max(0, Math.min(1, currentHp / maxHp)) : 0
    readonly property color faction: Theme.factionColor(isBlue)

    // 字号随行高缩放,但夹在 [0.85, 1.35]:行高由面板均分决定,不夹住会在
    // 大窗口下把编号顶成巨字、小窗口下缩到不可读。
    readonly property real k: Math.max(0.85, Math.min(1.35, height / 78))

    Rectangle {
        id: card
        anchors.fill: parent
        radius: 4
        // 边框照搬参考端 RobotStatsRow.qml:中性灰 #333333 细线 + 半透明近黑卡面,
        // 不按阵营染色、也不给自机单独一种颜色 —— 参考端花名册对每台车一视同仁,
        // 阵营已由血条颜色交代,再给边框上色就是同一条信息编码两次。
        border.color: root.online ? "#333333" : Qt.rgba(1, 1, 1, 0.07)
        border.width: 1
        color: Qt.rgba(0.1, 0.1, 0.15, 0.6)
    }

    // 顶栏同款机器人卡。关掉卡内窄血条,本行下方另画一条宽条。
    // 头像收到行高七成:原来贴到 card.height-8,几乎占满整行,把编号和血条挤到
    // 右半边,视觉重心全压在图上。头像是辅助识别,数据才是主角。
    RobotCard {
        id: portrait
        height: card.height * 0.7
        width: height * 82 / 72
        anchors.left: card.left
        anchors.leftMargin: 8
        anchors.verticalCenter: card.verticalCenter

        robotId: root.robotId
        isBlue: root.isBlue
        robotClass: root.robotClass
        currentHP: root.currentHp
        maxHP: root.maxHp
        hasHp: root.hasHp
        online: root.online
        showBar: false
        showId: false
    }

    // 字号层级:编号 13 / 兵种 11 / 血量 13 / 百分比 12。血量原本给到 15 且加粗,
    // 比编号还大,一行里出现两个「主标题」就会显得乱。编号是身份、血量是数值,
    // 同级即可,靠颜色区分而不是靠字号压过对方。
    Text {
        id: title
        // 只写编号数字,不带「号」:参考端 RobotStatsRow 的编号就是裸数字,
        // 「号」在一列全是机器人的面板里每行重复一次,是纯噪声。
        text: root.shownId
        color: root.online ? Theme.textPrimary : Theme.textMuted
        font.family: Theme.labelFamily
        font.pixelSize: Math.round(15 * root.k)
        font.bold: true
        font.letterSpacing: 0.8
        anchors.left: portrait.right
        anchors.leftMargin: 10
        anchors.top: card.top
        anchors.topMargin: Math.round(9 * root.k)
    }

    // 兵种名。顶栏受宽度限制只能给出编号,这里有空间就写全,
    // 让「头像是什么兵种」不必靠认图。
    Text {
        id: className
        text: root.robotClass
        color: root.online ? Theme.textSecondary : Theme.textMuted
        font.family: Theme.labelFamily
        font.pixelSize: Math.round(11 * root.k)
        font.letterSpacing: 0.5
        anchors.left: title.right
        anchors.leftMargin: 7
        anchors.baseline: title.baseline
    }

    // 当前血量单独一段:它是全行变化最快的数字,给足字重。上限值用次级色小一号,
    // 「578 / 600」里 600 是常量,不该和 578 一样抢眼。
    Row {
        id: hpText
        visible: root.hasHp && !root.noData
        spacing: 3
        anchors.left: title.left
        anchors.top: title.bottom
        anchors.topMargin: Math.round(4 * root.k)

        // 数字统一白色。参考端 RobotStatsRow 的 HP 文本就是 #FFFFFF,只有血条本身
        // 带颜色 —— 绿/黄数字配绿/黄血条等于同一条信息上两次色彩编码,
        // 面板里十行同时闪绿闪黄反而盖掉了真正该抢眼的告警。
        Text {
            text: root.currentHp
            color: root.kia ? Theme.textMuted : "#FFFFFF"
            font.family: Theme.numericFamily
            font.pixelSize: Math.round(14 * root.k)
            font.bold: true
        }
        Text {
            text: "/"
            color: Theme.textMuted
            font.family: Theme.numericFamily
            font.pixelSize: Math.round(11 * root.k)
            anchors.baseline: parent.children[0].baseline
        }
        Text {
            text: root.maxHp
            color: Theme.textMuted
            font.family: Theme.numericFamily
            font.pixelSize: Math.round(11 * root.k)
            anchors.baseline: parent.children[0].baseline
        }
    }

    Text {
        visible: root.kia
        text: "阵亡  KIA"
        color: Theme.danger
        font.family: Theme.labelFamily
        font.pixelSize: Math.round(12 * root.k)
        font.bold: true
        anchors.right: card.right
        anchors.rightMargin: 10
        anchors.verticalCenter: hpText.verticalCenter
    }

    Text {
        visible: !root.kia && root.hasHp
        text: Math.round(root.ratio * 100) + "%"
        color: Theme.textSecondary
        font.family: Theme.numericFamily
        font.pixelSize: Math.round(11 * root.k)
        font.bold: true
        anchors.right: card.right
        anchors.rightMargin: 10
        anchors.verticalCenter: hpText.verticalCenter
    }

    // 无数据:状态点 + 文字,绝不画空血条 —— 空条会被读成血量 0 即阵亡。
    Row {
        visible: root.noData
        anchors.left: title.left
        anchors.top: title.bottom
        anchors.topMargin: Math.round(5 * root.k)
        spacing: 5

        Rectangle {
            width: 7; height: 7; radius: 4
            anchors.verticalCenter: parent.verticalCenter
            color: root.hasPosition ? (root.positionStale ? Theme.warn : Theme.ok) : Theme.offline
        }
        Text {
            text: root.hasPosition
                ? (root.positionStale ? "仅位置 · 已过期" : "仅位置 · 无血量")
                : "无数据"
            color: Theme.textMuted
            font.family: Theme.labelFamily
            font.pixelSize: Math.round(11 * root.k)
        }
    }

    // 血条。高光用渐变现画,不能贴顶栏的 tab_bar.png:那张素材中间是一条
    // 完全不透明的亮红实心条(#F77563, alpha 255),顶栏之所以看不出来,是因为
    // 顶栏填充色本身就是阵营红、和纹理同色。这里填充是血量色(绿/黄),
    // 红纹理盖上去就会在绿条正中透出一条红线。高光必须由填充色自己派生。
    Item {
        id: bar
        visible: !root.noData
        anchors.left: title.left
        anchors.right: card.right
        anchors.rightMargin: 10
        anchors.bottom: card.bottom
        anchors.bottomMargin: Math.round(9 * root.k)
        height: Math.max(7, Math.round(9 * root.k))

        // 血条走阵营色,不走血量色 —— 照搬参考端 RobotStatsRow 的 hpColor:
        // 半血以上用队伍色,只在真正危险时(<=50% 橙 / <=25% 红)才改色。
        // 满血十台全绿会让「绿」失去含义;阵营色则同时交代了这是红方还是蓝方。
        readonly property color fill: {
            if (root.kia) return Theme.redDim;
            if (root.ratio >= 0.5) return root.faction;
            if (root.ratio > 0.25) return Theme.warn;
            return Theme.danger;
        }

        // 底槽:内凹感靠上深下浅,和顶栏的凹槽一致。
        Rectangle {
            anchors.fill: parent
            radius: 2
            color: "#0F1218"
            border.color: Qt.rgba(1, 1, 1, 0.06)
            border.width: 1
        }

        Rectangle {
            id: fillRect
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.margins: 1
            width: Math.max(0, Math.min(parent.width - 2, (parent.width - 2) * root.ratio))
            radius: 1
            clip: true

            // 顶亮底暗的竖向渐变 = 圆柱高光。同色系派生,所以任何血量色都成立。
            // 高光只到 1.08:1.28 会把压暗后的血量色重新提亮成荧光绿,
            // 等于在渲染层撤销 Theme 里的降饱和。
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.lighter(bar.fill, 1.08) }
                GradientStop { position: 0.45; color: bar.fill }
                GradientStop { position: 1.0; color: Qt.darker(bar.fill, 1.35) }
            }
        }

        // 血条前端亮边:让当前血量的位置有一个明确的读数点。
        Rectangle {
            visible: root.ratio > 0.01
            width: 1
            anchors.top: fillRect.top
            anchors.bottom: fillRect.bottom
            anchors.right: fillRect.right
            color: Qt.lighter(bar.fill, 1.25)
            opacity: 0.75
        }
    }
}
