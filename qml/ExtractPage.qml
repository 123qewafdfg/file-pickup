import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import FluentUI
import Cangjian

PageScroller {
    id: page

    // 同一口令再次提取时的确认弹窗
    Popup {
        id: dupPopup
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: 420
        modal: true
        focus: true
        padding: 0
        closePolicy: Popup.CloseOnEscape
        visible: App.duplicatePrompt
        onClosed: App.dismissDuplicatePrompt()

        // 遮罩用深色，避免浅色模式下整屏发白
        Overlay.modal: Rectangle {
            color: Qt.rgba(0, 0, 0, 0.45)
        }

        background: Rectangle {
            radius: 10
            color: FluTheme.dark ? "#2B2B2B" : "#FFFFFF"
            border.width: 1
            border.color: FluTheme.dividerColor
        }

        contentItem: ColumnLayout {
            spacing: 14

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 20
                Layout.bottomMargin: 0
                spacing: 12

                Rectangle {
                    Layout.preferredWidth: 36
                    Layout.preferredHeight: 36
                    radius: 18
                    color: Qt.rgba(FluTheme.primaryColor.r, FluTheme.primaryColor.g, FluTheme.primaryColor.b, 0.14)

                    FluIcon {
                        anchors.centerIn: parent
                        iconSource: FluentIcons.Info
                        iconSize: 18
                        iconColor: FluTheme.primaryColor
                    }
                }

                FluText {
                    Layout.fillWidth: true
                    text: "已经提取过了"
                    font: FluTextStyle.Title
                }
            }

            FluText {
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                text: App.duplicateMessage
                font: FluTextStyle.Body
                textColor: FluTheme.fontSecondaryColor
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 20
                Layout.topMargin: 4
                spacing: 10

                FluTextButton {
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredHeight: 34
                    visible: App.duplicateHasDir
                    text: "打开之前文件夹"
                    onClicked: App.openDuplicateDir()
                }

                Item { Layout.fillWidth: true }

                FluTextButton {
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredHeight: 34
                    Layout.preferredWidth: 88
                    text: "取消"
                    onClicked: App.dismissDuplicatePrompt()
                }

                FluFilledButton {
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredHeight: 34
                    Layout.preferredWidth: 100
                    text: "继续提取"
                    onClicked: App.continueAfterDuplicate()
                }
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: "选择提取保存位置"
        currentFolder: "file:///" + String(App.extractDir).replace(/\\\\/g, "/")
        onAccepted: App.setExtractDir(selectedFolder.toString())
    }

    pageContent: ColumnLayout {
        Layout.fillWidth: true
        spacing: 18

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6
            FluText {
                text: "提取"
                font: FluTextStyle.TitleLarge
            }
            FluText {
                text: "输入或粘贴含口令的文本即可取回内容；数字、中文和符号会自动忽略，口令大小写等价。"
                font: FluTextStyle.Body
                textColor: FluTheme.fontSecondaryColor
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: codeColumn.implicitHeight + 40
            radius: 8
            color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.045) : Qt.rgba(1, 1, 1, 0.7)
            border.width: 1
            border.color: FluTheme.dividerColor

            ColumnLayout {
                id: codeColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: 20
                spacing: 14

                FluText {
                    text: "口令码"
                    font: FluTextStyle.Caption
                    textColor: FluTheme.fontTertiaryColor
                }

                FluTextBox {
                    id: codeBox
                    Layout.fillWidth: true
                    Layout.preferredHeight: 56
                    text: App.code
                    placeholderText: "ABCDE"
                    maximumLength: 256
                    font: FluTextStyle.Title
                    leftPadding: 18
                    onTextEdited: App.code = text
                    Keys.onReturnPressed: App.startExtract()
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    FluTextButton {
                        text: "粘贴口令"
                        onClicked: App.pasteFromClipboard()
                    }

                    FluTextButton {
                        text: "清空"
                        visible: App.code.length > 0
                        onClicked: App.code = ""
                    }

                    Item { Layout.fillWidth: true }

                    FluText {
                        visible: App.code.length > 0 && !/^[A-Za-z]{5}$/.test(App.code)
                        text: /^[A-Za-z]*$/.test(App.code) && App.code.length < 5
                              ? "还需 " + (5 - App.code.length) + " 个字母"
                              : "口令必须恰好是 5 位英文字母"
                        font: FluTextStyle.Caption
                        textColor: "#C42B1C"
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            FluFilledButton {
                text: App.busy ? "正在提取…" : "提取内容"
                disabled: !/^[A-Za-z]{5}$/.test(App.code) || App.busy
                Layout.preferredWidth: 140
                Layout.preferredHeight: 36
                onClicked: App.startExtract()
            }

            FluText {
                Layout.fillWidth: true
                text: App.busy ? App.stage : "平台内容被删除后，将无法恢复。"
                font: FluTextStyle.Caption
                textColor: FluTheme.fontTertiaryColor
                elide: Text.ElideRight
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6
            visible: App.busy && App.uploadSummary.length > 0

            RowLayout {
                Layout.fillWidth: true
                FluText {
                    text: App.uploadSummary
                    font: FluTextStyle.Caption
                    textColor: FluTheme.fontSecondaryColor
                }
                Item { Layout.fillWidth: true }
                FluText {
                    text: App.progress + "%"
                    font: FluTextStyle.Caption
                    textColor: FluTheme.fontSecondaryColor
                }
            }
            FluProgressBar {
                Layout.fillWidth: true
                from: 0
                to: 1
                value: Math.max(0, Math.min(100, App.progress)) / 100
                Accessible.name: App.uploadSummary
                Behavior on value {
                    NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
                }
            }
        }

        // 提取保存位置：每次提取自动建时间戳子文件夹，根目录可自定义
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: saveColumn.implicitHeight + 24
            radius: 8
            color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.05) : Qt.rgba(0, 0, 0, 0.03)
            border.width: 1
            border.color: FluTheme.dividerColor

            ColumnLayout {
                id: saveColumn
                anchors.fill: parent
                anchors.margins: 12
                spacing: 10

                FluText {
                    text: "提取保存位置"
                    font: FluTextStyle.BodyStrong
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    FluTextBox {
                        id: dirBox
                        Layout.fillWidth: true
                        readOnly: true
                        text: App.extractDir
                        placeholderText: "选择保存到哪个文件夹"
                    }

                    FluTextButton {
                        text: "选择其他…"
                        onClicked: folderDialog.open()
                    }
                    FluTextButton {
                        text: "打开"
                        onClicked: App.openExtractDir()
                    }
                    FluTextButton {
                        text: "清理"
                        onClicked: App.clearExtractDir()
                    }
                }

                FluText {
                    Layout.fillWidth: true
                    text: "每次提取会在该目录下新建一个时间戳文件夹（如 20261022-213015），上一次的结果不会被覆盖。"
                    font: FluTextStyle.Caption
                    textColor: FluTheme.fontTertiaryColor
                    wrapMode: Text.WordWrap
                }
            }
        }

    }
}
