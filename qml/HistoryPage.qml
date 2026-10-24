import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import FluentUI
import Cangjian

// 历史记录：固定头部 + ListView 懒加载（只实例化可见行），不再整体铺开
Item {
    id: page

    function playEnterAnimation() {
        if (!page.visible)
            return
        if (!FluTheme.animationEnabled) {
            historyContent.y = 0
            historyContent.opacity = 1
            return
        }
        historyEnterAnimation.restart()
    }

    onVisibleChanged: {
        if (visible)
            Qt.callLater(page.playEnterAnimation)
        else
            historyEnterAnimation.stop()
    }

    property string filter: "全部"
    property int filterIndex: 0
    property var items: []
    property bool confirmingClear: false

    readonly property var filterNames: ["全部", "制作", "提取"]

    function refresh() {
        const src = App.history
        const out = []
        for (let i = 0; i < src.length; ++i) {
            const entry = src[i]
            if (page.filter === "全部" || entry.kind === page.filter)
                out.push(entry)
        }
        page.items = out
    }

    Component.onCompleted: {
        refresh()
        Qt.callLater(page.playEnterAnimation)
    }
    onFilterChanged: {
        refresh()
        confirmingClear = false
    }

    Connections {
        target: App
        function onHistoryChanged() { page.refresh() }
    }

    Item {
        id: historyContent
        x: 0
        y: 0
        width: page.width
        height: page.height
        opacity: 1

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 36
        anchors.rightMargin: 28
        anchors.topMargin: 36
        spacing: 14

        // ---- 头部（固定）----
        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4
                FluText {
                    text: "历史记录"
                    font: FluTextStyle.TitleLarge
                }
                FluText {
                    text: "制作与提取记录保存在本机，不上传。"
                    font: FluTextStyle.Body
                    textColor: FluTheme.fontSecondaryColor
                }
            }

            FluText {
                text: "共 " + App.history.length + " 条"
                font: FluTextStyle.Caption
                textColor: FluTheme.fontTertiaryColor
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Repeater {
                model: page.filterNames
                delegate: Button {
                    readonly property bool active: page.filterIndex === index
                    implicitHeight: 30
                    implicitWidth: chipText.implicitWidth + 28
                    padding: 0
                    onClicked: {
                        page.filterIndex = index
                        page.filter = modelData
                    }

                    background: Rectangle {
                        radius: 15
                        color: active
                               ? Qt.rgba(FluTheme.primaryColor.r, FluTheme.primaryColor.g, FluTheme.primaryColor.b, 0.14)
                               : "transparent"
                        border.width: 1
                        border.color: active ? FluTheme.primaryColor : FluTheme.dividerColor
                    }

                    contentItem: FluText {
                        id: chipText
                        text: modelData
                        font: FluTextStyle.Caption
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        textColor: active ? FluTheme.primaryColor : FluTheme.fontSecondaryColor
                    }
                }
            }

            Item { Layout.fillWidth: true }

            FluTextButton {
                visible: App.history.length > 0 && !page.confirmingClear
                text: "清空记录"
                onClicked: page.confirmingClear = true
            }
            FluTextButton {
                visible: page.confirmingClear
                text: "确认清空"
                textColor: "#C42B1C"
                onClicked: {
                    page.confirmingClear = false
                    App.clearHistory()
                }
            }
            FluTextButton {
                visible: page.confirmingClear
                text: "取消"
                onClicked: page.confirmingClear = false
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: FluTheme.dividerColor
        }

        // ---- 列表（懒加载）----
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            visible: page.items.length > 0
            model: page.items
            spacing: 8
            boundsBehavior: Flickable.StopAtBounds
            cacheBuffer: 144
            reuseItems: true
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                width: list.width - 12
                height: 72
                radius: 8
                color: hover.containsMouse
                       ? (FluTheme.dark ? Qt.rgba(1, 1, 1, 0.06) : Qt.rgba(0, 0, 0, 0.035))
                       : (FluTheme.dark ? Qt.rgba(1, 1, 1, 0.03) : Qt.rgba(1, 1, 1, 0.6))
                border.width: 1
                border.color: FluTheme.dividerColor

                MouseArea {
                    id: hover
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.NoButton
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 12
                    spacing: 12

                    Rectangle {
                        Layout.preferredWidth: 34
                        Layout.preferredHeight: 34
                        radius: 8
                        color: modelData.kind === "制作"
                               ? Qt.rgba(FluTheme.primaryColor.r, FluTheme.primaryColor.g, FluTheme.primaryColor.b, 0.12)
                               : (FluTheme.dark ? Qt.rgba(1, 1, 1, 0.06) : Qt.rgba(0, 0, 0, 0.04))

                        FluIcon {
                            anchors.centerIn: parent
                            iconSource: modelData.kind === "制作" ? FluentIcons.Send : FluentIcons.Link
                            iconSize: 15
                            iconColor: modelData.kind === "制作" ? FluTheme.primaryColor : FluTheme.fontSecondaryColor
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 3

                        FluText {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: modelData.name
                            font: FluTextStyle.Body
                            elide: Text.ElideMiddle
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 8

                            FluText {
                                text: modelData.time
                                font: FluTextStyle.Caption
                                textColor: FluTheme.fontTertiaryColor
                            }

                            Rectangle {
                                width: 3
                                height: 3
                                radius: 1.5
                                color: FluTheme.fontTertiaryColor
                            }

                            FluText {
                                text: modelData.size !== undefined && modelData.size > 0
                                      ? App.humanSize(modelData.size)
                                      : "—"
                                font: FluTextStyle.Caption
                                textColor: FluTheme.fontTertiaryColor
                            }

                            Rectangle {
                                width: 3
                                height: 3
                                radius: 1.5
                                color: FluTheme.fontTertiaryColor
                            }

                            FluText {
                                text: modelData.ok ? "完成" : "失败"
                                font: FluTextStyle.Caption
                                textColor: modelData.ok ? FluTheme.fontTertiaryColor : "#C42B1C"
                            }
                        }
                    }

                    Item {
                        Layout.preferredWidth: 104
                        Layout.preferredHeight: 32

                        Button {
                            id: codeButton
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            width: codeRow.implicitWidth + 18
                            height: 28
                            visible: modelData.code !== undefined
                                     && String(modelData.code).length > 0
                            padding: 0
                            text: "复制取件码 " + modelData.code
                            Accessible.name: text

                            background: Rectangle {
                                radius: 6
                                color: codeButton.hovered
                                       ? Qt.rgba(FluTheme.primaryColor.r, FluTheme.primaryColor.g,
                                                 FluTheme.primaryColor.b, 0.12)
                                       : (FluTheme.dark ? Qt.rgba(1, 1, 1, 0.07)
                                                        : Qt.rgba(0, 0, 0, 0.05))
                                border.width: 1
                                border.color: codeButton.activeFocus
                                              ? FluTheme.primaryColor : FluTheme.dividerColor
                            }

                            contentItem: Item {
                                Row {
                                    id: codeRow
                                    anchors.centerIn: parent
                                    spacing: 6

                                    FluText {
                                        text: modelData.code
                                        font.family: "Consolas"
                                        font.pixelSize: 14
                                        font.weight: Font.DemiBold
                                        font.letterSpacing: 0.6
                                        textColor: FluTheme.primaryColor
                                    }
                                    FluIcon {
                                        anchors.verticalCenter: parent.verticalCenter
                                        iconSource: FluentIcons.Copy
                                        iconSize: 11
                                        iconColor: FluTheme.fontSecondaryColor
                                    }
                                }
                            }
                            onClicked: App.copyText(modelData.code)
                        }
                    }

                    Item {
                        Layout.preferredWidth: 96
                        Layout.preferredHeight: 32

                        FluTextButton {
                            anchors.fill: parent
                            visible: modelData.kind === "提取"
                                     && modelData.dir !== undefined
                                     && String(modelData.dir).length > 0
                            text: "打开文件夹"
                            onClicked: App.openPath(modelData.dir)
                        }
                    }
                }
            }
        }

        // ---- 空状态 ----
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 8
            visible: page.items.length === 0

            Item { Layout.fillHeight: true }
            FluIcon {
                Layout.alignment: Qt.AlignHCenter
                iconSource: FluentIcons.History
                iconSize: 28
                iconColor: FluTheme.fontTertiaryColor
            }
            FluText {
                Layout.fillWidth: true
                text: App.history.length === 0
                      ? "还没有记录，完成一次制作或提取后会自动写入本机。"
                      : "该分类下暂无记录。"
                font: FluTextStyle.Caption
                textColor: FluTheme.fontTertiaryColor
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
            Item { Layout.fillHeight: true }
        }
    }
    }

    ParallelAnimation {
        id: historyEnterAnimation
        YAnimator {
            target: historyContent
            from: 42
            to: 0
            duration: 167
            easing.type: Easing.OutCubic
        }
        OpacityAnimator {
            target: historyContent
            from: 0
            to: 1
            duration: 110
            easing.type: Easing.OutCubic
        }
    }
}
