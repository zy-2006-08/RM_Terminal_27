import QtQuick
import "."

// 结算动画。复刻上游 GameResultWidget 的观感:逐帧序列 + 从 50% 弹到 100% 的放大。
//
// 用序列帧而不是自绘,是因为上游那段光效是美术逐帧渲出来的,任何代码画法都做不出
// 同样的质感。131 帧走磁盘按需加载,不进二进制。
Item {
    id: root

    // C++ 注入完整帧 URL 列表。素材的序号**不连续**(红胜 17~174 之间缺 27 个),
    // 按 first..last 自增会请求到不存在的文件而卡帧,所以帧清单只能由 C++ 枚举
    // 真实目录后给出,QML 不自己拼序号。列表为空表示这种结果没有逐帧素材(平局)。
    property string title: ""
    property var frameUrls: []
    property bool shown: false

    visible: shown

    readonly property bool hasFrames: frameUrls.length > 0

    // 上游 m_frameTimer->start(10):每 10ms 推一帧。
    readonly property int frameIntervalMs: 10
    property int frameIndex: 0

    // 压暗层。结算是全场唯一「可以完全接管屏幕」的时刻,所以压得比普通弹窗更狠。
    Rectangle {
        anchors.fill: parent
        color: "#B3000000"
    }

    Item {
        id: stage
        anchors.centerIn: parent
        width: parent.width
        height: parent.height

        // 上游 QPropertyAnimation geometry 0.5→1.0,OutBack 带回弹,800ms。
        // QML 里用 scale 表达同一条曲线,效果一致但不触发布局重算。
        scale: 0.5
        opacity: 0

        Image {
            id: frameImage
            anchors.fill: parent
            visible: root.hasFrames
            fillMode: Image.PreserveAspectFit
            asynchronous: false
            cache: false
            source: root.hasFrames && root.frameIndex < root.frameUrls.length
                        ? root.frameUrls[root.frameIndex]
                        : ""
        }

        // 平局没有序列帧,以及序列帧加载失败时的兜底:结算结果必须说出来,
        // 不能因为缺图就只剩一片黑。
        Text {
            anchors.centerIn: parent
            visible: !root.hasFrames || frameImage.status === Image.Error
            text: root.title
            color: Theme.textPrimary
            font.family: Theme.titleFamily
            font.pixelSize: 64
            font.bold: true
            font.letterSpacing: Theme.titleSpacing * 2
        }
    }

    // 播放器。shown 翻真时从首帧开始推进,到末帧停住(上游同样停在末帧而不是循环)。
    Timer {
        id: player
        interval: root.frameIntervalMs
        repeat: true
        running: false
        onTriggered: {
            // 停在末帧而不是循环,与上游一致:结算画面要留在屏幕上。
            if (root.frameIndex + 1 >= root.frameUrls.length) {
                player.stop()
                return
            }
            root.frameIndex = root.frameIndex + 1
        }
    }

    ParallelAnimation {
        id: entrance
        NumberAnimation {
            target: stage
            property: "scale"
            from: 0.5
            to: 1.0
            duration: 800
            easing.type: Easing.OutBack
        }
        NumberAnimation {
            target: stage
            property: "opacity"
            from: 0
            to: 1.0
            duration: 320
        }
    }

    onShownChanged: {
        if (shown) {
            frameIndex = 0
            entrance.restart()
            if (hasFrames) player.restart()
        } else {
            player.stop()
            entrance.stop()
            stage.scale = 0.5
            stage.opacity = 0
        }
    }
}
