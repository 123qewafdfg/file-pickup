import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import FluentUI

// 折叠列表（渐进式展开，参考 NN/g progressive disclosure）：
// - 收起时只显示前 collapsedRows 行
// - 展开后高度封顶 maxRows 行，超出部分在列表内部滚动
// - 展开/收起按钮固定在列表正下方，不随页面滚到最底部
// - 展开时列表会主动向下滑动一行（scrollOnExpand），提示“下面还有内容”，这是刻意设计
ColumnLayout {
    id: root

    property var items: []
    property Component rowDelegate
    property int collapsedRows: 3
    property int maxRows: 6
    property int rowHeight: 56
    property bool expanded: false
    // 展开时向下滑动的行数，0 表示不滑
    property int scrollRowsOnExpand: 1
    property int scrollDuration: 220

    readonly property int total: items ? items.length : 0
    readonly property int shownRows: expanded
                                    ? Math.min(total, maxRows)
                                    : Math.min(total, collapsedRows)

    spacing: 6

    ListView {
        id: list
        Layout.fillWidth: true
        Layout.preferredHeight: root.shownRows * root.rowHeight
        visible: root.total > 0
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: root.items
        delegate: root.rowDelegate
        // 内容超出可视高度时，滚动条常显（否则只有滑动时才闪一下）
        readonly property bool scrollable: contentHeight > height
        ScrollBar.vertical: ScrollBar {
            policy: list.scrollable ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
            width: 8
            active: true
        }

        // 刻意为之：展开后主动下滑，露出被折叠的内容；收起时回到顶部
        NumberAnimation on contentY {
            id: nudge
            duration: root.scrollDuration
            easing.type: Easing.OutCubic
        }
    }

    // 用行数直接算偏移：展开瞬间 ListView 的 contentHeight 还没更新，读它会得到 0
    function applyScroll() {
        const rows = root.expanded
                     ? Math.min(root.total, root.maxRows)
                     : Math.min(root.total, root.collapsedRows)
        const maxOffset = Math.max(0, (root.total - rows) * root.rowHeight)
        const target = root.expanded
                       ? Math.min(maxOffset, root.scrollRowsOnExpand * root.rowHeight)
                       : 0
        nudge.stop()
        nudge.to = target
        nudge.start()
    }

    onExpandedChanged: Qt.callLater(root.applyScroll)

    // 按钮常驻：收起时在第三行下方，展开后仍在列表下方
    FluTextButton {
        Layout.alignment: Qt.AlignHCenter
        visible: root.total > root.collapsedRows
        text: root.expanded
              ? "收起（共 " + root.total + " 项）"
              : "展开全部 " + root.total + " 项"
        onClicked: root.expanded = !root.expanded
    }
}
