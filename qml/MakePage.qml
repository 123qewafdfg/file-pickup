import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Qt5Compat.GraphicalEffects
import FluentUI
import Cangjian

PageScroller {
    id: page

    property bool filesExpanded: false

    pageContent: ColumnLayout {
        Layout.fillWidth: true
        spacing: 18

        // ---------- 标题 ----------
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6

            FluText {
                text: "制作"
                font: FluTextStyle.TitleLarge
            }
            FluText {
                text: "选择要隐藏的内容；选定平台并点击发布后才会开始本地加密，原文件不会被修改。"
                font: FluTextStyle.Body
                textColor: FluTheme.fontSecondaryColor
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        // ---------- 添加区 ----------
        Rectangle {
            id: dropArea
            Layout.fillWidth: true
            Layout.preferredHeight: App.files.length === 0 ? 176 : 84
            radius: 8
            color: dragContains
                   ? (FluTheme.dark ? Qt.rgba(1, 1, 1, 0.09) : Qt.rgba(0, 0, 0, 0.03))
                   : "transparent"
            border.width: 1
            border.color: dragContains ? FluTheme.primaryColor : FluTheme.dividerColor

            property bool dragContains: false

            ColumnLayout {
                anchors.centerIn: parent
                spacing: 10

                FluIcon {
                    iconSource: FluentIcons.Add
                    iconSize: App.files.length === 0 ? 30 : 20
                    iconColor: FluTheme.primaryColor
                    Layout.alignment: Qt.AlignHCenter
                }

                FluText {
                    text: App.files.length === 0 ? "点击添加，或把文件拖到这里" : "继续添加内容"
                    font: FluTextStyle.Body
                    textColor: FluTheme.fontSecondaryColor
                    Layout.alignment: Qt.AlignHCenter
                }

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 8
                    visible: App.files.length === 0

                    FluButton { text: "图片"; onClicked: imageDialog.open() }
                    FluButton { text: "视频"; onClicked: videoDialog.open() }
                    FluButton { text: "文件"; onClicked: anyDialog.open() }
                    FluButton { text: "文字"; onClicked: textDialog.open() }
                }
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                onClicked: anyDialog.open()
            }

            DropArea {
                anchors.fill: parent
                onEntered: dropArea.dragContains = true
                onExited: dropArea.dragContains = false
                onDropped: function(drop) {
                    dropArea.dragContains = false
                    var list = []
                    for (var i = 0; i < drop.urls.length; ++i)
                        list.push(drop.urls[i].toString())
                    App.addUrls(list)
                }
            }
        }

        // ---------- 已选内容 ----------
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4
            visible: App.files.length > 0

            RowLayout {
                Layout.fillWidth: true
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    FluText {
                        text: "已添加 " + App.files.length + " 项 · 合计 " + App.filesTotalText
                        font: FluTextStyle.BodyStrong
                        Layout.fillWidth: true
                        elide: Text.ElideRight
Layout.minimumWidth: 0
                    }
                    FluText {
                        text: "合计按压缩后体积计算，未压缩的文件按原体积计入"
                        font: FluTextStyle.Caption
                        textColor: FluTheme.fontTertiaryColor
                        Layout.fillWidth: true
                        elide: Text.ElideRight
Layout.minimumWidth: 0
                    }
                }
                FluTextButton {
                    text: "清空"
                    onClicked: App.clearFiles()
                }
            }

            // 折叠列表：自适应高度 + 内部滚动，展开/收起按钮固定在列表下方
            CollapsibleList {
                Layout.fillWidth: true
                items: App.files

                rowDelegate: Component {
                    Item {
                        width: ListView.view ? ListView.view.width : 0
                        height: 56

                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: App.openPath(modelData.path)
                        }

                        Rectangle {
                            anchors.fill: parent
                            radius: 6
                            color: rowMouse.containsMouse
                                   ? (FluTheme.dark ? Qt.rgba(1, 1, 1, 0.04)
                                                    : Qt.rgba(0, 0, 0, 0.025))
                                   : "transparent"
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 4
                            spacing: 12

                            // 图片显示缩略图，其它类型显示图标
                            Rectangle {
                                Layout.preferredWidth: 40
                                Layout.preferredHeight: 40
                                radius: 6
                                color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.06) : Qt.rgba(0, 0, 0, 0.04)

                                Image {
                                    id: thumbSource
                                    visible: false
                                    anchors.fill: parent
                                    source: modelData.kind === "图片"
                                            ? "file:///" + modelData.path : ""
                                    sourceSize: Qt.size(80, 80)
                                    fillMode: Image.PreserveAspectCrop
                                    asynchronous: true
                                    cache: true
                                }

                                OpacityMask {
                                    anchors.fill: parent
                                    visible: thumbSource.status === Image.Ready
                                    source: thumbSource
                                    maskSource: Rectangle {
                                        width: thumbSource.width
                                        height: thumbSource.height
                                        radius: 6
                                    }
                                }

                                FluIcon {
                                    anchors.centerIn: parent
                                    visible: thumbSource.status !== Image.Ready
                                    iconSource: modelData.kind === "图片" ? FluentIcons.Photo
                                              : modelData.kind === "视频" ? FluentIcons.Video
                                              : modelData.kind === "文字" ? FluentIcons.Edit
                                              : FluentIcons.Document
                                    iconSize: 17
                                    iconColor: FluTheme.fontSecondaryColor
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                FluText {
                                    text: modelData.name
                                    font: FluTextStyle.Body
                                    Layout.fillWidth: true
                                    elide: Text.ElideMiddle
Layout.minimumWidth: 0
                                }
                                FluText {
                                    text: modelData.meta
                                    font: FluTextStyle.Caption
                                    textColor: FluTheme.fontTertiaryColor
                                }
                            }

                            FluIconButton {
                                iconSource: FluentIcons.Delete
                                iconSize: 15
                                implicitWidth: 32
                                implicitHeight: 32
                                ToolTip.text: "移除"
                                ToolTip.visible: hovered
                                ToolTip.delay: 600
                                onClicked: App.removeAt(index)
                            }
                        }
                    }
                }
            }
        }

        // ---------- 设置 ----------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: settingsColumn.implicitHeight + 20
            radius: 8
            color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.045) : Qt.rgba(1, 1, 1, 0.7)
            border.width: 1
            border.color: FluTheme.dividerColor

            Column {
                id: settingsColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: 10
                spacing: 0

                SettingLine {
                    width: parent.width
                    iconSource: FluentIcons.Photo
                    title: "图片压缩"
                    detail: "静图转 WebP（质量 75），更小才采用"
                    checked: App.compressImages
                    onToggled: function(value) { App.compressImages = value }
                }
                SettingLine {
                    width: parent.width
                    iconSource: FluentIcons.Shield
                    title: "完整校验"
                    detail: "生成后回读校验载体可恢复"
                    checked: App.fullCheck
                    onToggled: function(value) { App.fullCheck = value }
                }
            }
        }

        // ---------- 进度 ----------
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            visible: App.busy

            FluProgressRing {
                Layout.preferredWidth: 18
                Layout.preferredHeight: 18
            }

            FluText {
                text: App.uploadSummary.length > 0 ? App.stage + " · " + App.uploadSummary : App.stage
                font: FluTextStyle.Caption
                textColor: FluTheme.fontSecondaryColor
                Layout.fillWidth: true
            }
        }

        FluProgressBar {
            Layout.fillWidth: true
            Layout.preferredHeight: 6
            visible: App.busy && App.progress > 0
            indeterminate: false
            strokeWidth: 6
            value: Math.max(0, Math.min(100, App.progress)) / 100
        }

        // 导入即转换的独立进度
        FluProgressBar {
            Layout.fillWidth: true
            Layout.preferredHeight: 6
            visible: App.compressBusy
            indeterminate: false
            strokeWidth: 6
            value: Math.max(0, Math.min(100, App.compressProgress)) / 100
        }

        FluText {
            Layout.fillWidth: true
            visible: App.compressInfo.length > 0
            text: App.compressInfo
            font: FluTextStyle.Caption
            textColor: FluTheme.fontSecondaryColor
            wrapMode: Text.WrapAnywhere
        }

        // ---------- 底部操作 ----------
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 6
            spacing: 12

            FluFilledButton {
                text: App.busy ? "处理中…" : "选择发放平台"
                disabled: App.files.length === 0 || App.busy
                Layout.preferredWidth: 168
                Layout.preferredHeight: 36
                onClicked: Router.go("platform")
            }

            FluText {
                text: App.files.length === 0 ? "还没有添加内容" : "已选 " + App.files.length + " 项"
                font: FluTextStyle.Caption
                textColor: FluTheme.fontTertiaryColor
                Layout.fillWidth: true
            }
        }
    }

    FileDialog {
        id: imageDialog
        title: "选择图片（可多选）"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["图片 (*.png *.jpg *.jpeg *.webp *.gif *.bmp *.heic *.heif)"]
        onAccepted: App.addUrls(selectedFiles.map(function(f) { return f.toString() }))
    }

    FileDialog {
        id: videoDialog
        title: "选择视频（可多选）"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["视频 (*.mp4 *.mov *.mkv *.webm *.avi *.m4v)"]
        onAccepted: App.addUrls(selectedFiles.map(function(f) { return f.toString() }))
    }

    FileDialog {
        id: anyDialog
        title: "选择文件（可多选）"
        fileMode: FileDialog.OpenFiles
        onAccepted: App.addUrls(selectedFiles.map(function(f) { return f.toString() }))
    }

    Dialog {
        id: textDialog
        title: "添加文字"
        modal: true
        anchors.centerIn: parent
        width: 560
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: {
            App.addText(textArea.text)
            textArea.text = ""
        }

        contentItem: ColumnLayout {
            spacing: 10
            FluText {
                text: "要隐藏的文字（不超过 1000 字节）"
                font: FluTextStyle.Caption
                textColor: FluTheme.fontSecondaryColor
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 160
                FluMultilineTextBox {
                    id: textArea
                    placeholderText: "在这里输入…"
                    wrapMode: TextArea.Wrap
                }
            }
        }
    }
}
