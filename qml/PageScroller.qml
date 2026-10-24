import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import FluentUI

// 滚动页容器：内容宽度固定上限并在左侧起点对齐，避免大窗口下被拉伸
FluPage {
    id: root

    // FluPage's built-in Behaviors use GUI-thread NumberAnimation. Animate
    // an inner, non-layout-managed layer with scene-graph Animators instead.
    animationEnabled: false

    default property alias pageContent: column.data

    property int pageMargin: 36
    property int maxContentWidth: 880
    property int itemSpacing: 18

    function playEnterAnimation() {
        if (!root.visible)
            return
        if (!FluTheme.animationEnabled) {
            motionLayer.y = 0
            motionLayer.opacity = 1
            return
        }
        enterAnimation.restart()
    }

    onVisibleChanged: {
        if (visible)
            Qt.callLater(root.playEnterAnimation)
        else
            enterAnimation.stop()
    }
    Component.onCompleted: Qt.callLater(root.playEnterAnimation)

    Item {
        id: motionLayer
        x: 0
        y: 0
        width: root.width
        height: root.height
        opacity: 1

        Flickable {
            id: flick
            anchors.fill: parent
            clip: true
            contentWidth: width
            contentHeight: column.implicitHeight + root.pageMargin * 2
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: FluScrollBar { }

            ColumnLayout {
                id: column
                x: root.pageMargin
                y: root.pageMargin
                width: Math.min(flick.width - root.pageMargin * 2 - 12, root.maxContentWidth)
                spacing: root.itemSpacing
            }
        }
    }

    ParallelAnimation {
        id: enterAnimation
        YAnimator {
            target: motionLayer
            from: 42
            to: 0
            duration: 167
            easing.type: Easing.OutCubic
        }
        OpacityAnimator {
            target: motionLayer
            from: 0
            to: 1
            duration: 110
            easing.type: Easing.OutCubic
        }
    }
}
