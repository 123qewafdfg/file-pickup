import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import FluentUI
import Qt5Compat.GraphicalEffects
import Cangjian

PageScroller {
    id: page

    pageContent: ColumnLayout {
        Layout.fillWidth: true
        spacing: 18

        RowLayout {
            Layout.fillWidth: true
            spacing: 14

            Rectangle {
                Layout.preferredWidth: 44
                Layout.preferredHeight: 44
                radius: 22
                color: FluTheme.primaryColor

                FluIcon {
                    anchors.centerIn: parent
                    iconSource: FluentIcons.CheckMark
                    iconSize: 20
                    iconColor: FluTheme.dark ? "#000000" : "#FFFFFF"
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3
                FluText {
                    text: "提取结果"
                    font: FluTextStyle.TitleLarge
                }
                FluText {
                    text: "已恢复 " + App.outputs.length + " 个文件，完整性校验通过"
                    font: FluTextStyle.Body
                    textColor: FluTheme.fontSecondaryColor
                }
            }
        }

        // 保存位置：每次提取一个时间戳文件夹，可直接打开
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 52
            radius: 8
            color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.05) : Qt.rgba(0, 0, 0, 0.03)
            border.width: 1
            border.color: FluTheme.dividerColor

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 10

                FluIcon {
                    iconSource: FluentIcons.FolderOpen
                    iconSize: 16
                    iconColor: FluTheme.fontSecondaryColor
                }
                FluText {
                    Layout.fillWidth: true
                    text: App.extractSessionDir
                    font: FluTextStyle.Caption
                    textColor: FluTheme.fontSecondaryColor
                    elide: Text.ElideMiddle
Layout.minimumWidth: 0
                }
            }
        }

        // 提取结果同样折叠：超过 3 项先收起，按钮固定在列表下方
        CollapsibleList {
            Layout.fillWidth: true
            items: App.outputs
            rowHeight: 68

            rowDelegate: Component {
                Rectangle {
                    width: ListView.view ? ListView.view.width : 0
                    height: 68
                    radius: 8
                    color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.045) : Qt.rgba(1, 1, 1, 0.7)
                    border.width: 1
                    border.color: FluTheme.dividerColor

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 8
                        spacing: 12

                        // 缩略图（图片）或类型图标
                        Rectangle {
                            Layout.preferredWidth: 44
                            Layout.preferredHeight: 44
                            radius: 6
                            color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.06) : Qt.rgba(0, 0, 0, 0.04)

                            Image {
                                id: outputThumb
                                visible: false
                                anchors.fill: parent
                                source: modelData.kind === "图片" ? "file:///" + modelData.path : ""
                                sourceSize: Qt.size(88, 88)
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                cache: true
                            }

                            OpacityMask {
                                anchors.fill: parent
                                visible: outputThumb.status === Image.Ready
                                source: outputThumb
                                maskSource: Rectangle {
                                    width: outputThumb.width
                                    height: outputThumb.height
                                    radius: 6
                                }
                            }

                            FluIcon {
                                anchors.centerIn: parent
                                visible: outputThumb.status !== Image.Ready
                                iconSource: modelData.kind === "图片" ? FluentIcons.Photo
                                          : modelData.kind === "视频" ? FluentIcons.Video
                                          : modelData.kind === "文字" ? FluentIcons.Edit
                                          : FluentIcons.Document
                                iconSize: 18
                                iconColor: FluTheme.fontSecondaryColor
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            FluText {
                                Layout.fillWidth: true
                                text: modelData.name
                                font: FluTextStyle.Body
                                elide: Text.ElideMiddle
Layout.minimumWidth: 0
                            }
                            FluText {
                                Layout.fillWidth: true
                                text: modelData.meta
                                font: FluTextStyle.Caption
                                textColor: FluTheme.fontTertiaryColor
                                elide: Text.ElideRight
Layout.minimumWidth: 0
                            }
                        }

                        FluButton {
                            text: "打开"
                            Layout.preferredWidth: 84
                            Layout.preferredHeight: 32
                            onClicked: App.openPath(modelData.path)
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 6
            spacing: 10

            FluFilledButton {
                text: "提取其他口令"
                Layout.preferredWidth: 148
                Layout.preferredHeight: 36
                onClicked: {
                    App.resetExtract()
                    Router.go("extract")
                }
            }

            FluButton {
                text: "打开所在文件夹"
                Layout.preferredWidth: 148
                Layout.preferredHeight: 36
                enabled: App.outputs.length > 0
                onClicked: App.revealPath(App.outputs.length > 0 ? App.outputs[0].path : "")
            }
        }
    }
}
