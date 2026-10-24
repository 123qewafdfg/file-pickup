import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import FluentUI
import Cangjian

PageScroller {
    id: page

    pageContent: ColumnLayout {
        Layout.fillWidth: true
        spacing: 18

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6
            FluText {
                text: "选择发放平台"
                font: FluTextStyle.TitleLarge
            }
            FluText {
                text: "口令会登记到文件取件后端，载体由平台承载。已选 " + App.files.length + " 项内容。"
                font: FluTextStyle.Body
                textColor: FluTheme.fontSecondaryColor
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        Repeater {
            model: App.accounts
            delegate: Rectangle {
                id: platformCard
                Layout.fillWidth: true
                Layout.preferredHeight: 76
                radius: 8
                color: App.platform === modelData.name
                       ? (FluTheme.dark ? Qt.rgba(1, 1, 1, 0.07) : Qt.rgba(0, 0, 0, 0.02))
                       : "transparent"
                border.width: 1
                border.color: App.platform === modelData.name
                              ? FluTheme.primaryColor
                              : FluTheme.dividerColor

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    spacing: 14

                    PlatformLogo {
                        Layout.preferredWidth: 40
                        Layout.preferredHeight: 40
                        platform: modelData.name
                    }

                    Item {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 42

                        Column {
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2

                            Row {
                                spacing: 8
                                FluText {
                                    text: modelData.name
                                    font: FluTextStyle.BodyStrong
                                }
                                Rectangle {
                                    visible: modelData.recommended
                                    width: visible ? recommendLabel.implicitWidth + 10 : 0
                                    height: 18
                                    radius: 3
                                    color: FluTheme.primaryColor
                                    opacity: 0.16

                                    FluText {
                                        id: recommendLabel
                                        anchors.centerIn: parent
                                        text: "推荐"
                                        font: FluTextStyle.Caption
                                    }
                                }
                            }

                            FluText {
                                text: modelData.connected ? "已连接" : modelData.detail
                                font: FluTextStyle.Caption
                                textColor: FluTheme.fontTertiaryColor
                            }
                        }
                    }

                    Rectangle {
                        Layout.preferredWidth: 18
                        Layout.preferredHeight: 18
                        radius: 9
                        color: "transparent"
                        border.width: 1
                        border.color: App.platform === modelData.name
                                      ? FluTheme.primaryColor
                                      : FluTheme.dividerColor

                        Rectangle {
                            anchors.centerIn: parent
                            width: 8
                            height: 8
                            radius: 4
                            color: FluTheme.primaryColor
                            visible: App.platform === modelData.name
                        }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: App.platform = modelData.name
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: notice.implicitHeight + 24
            radius: 8
            color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.05) : Qt.rgba(0, 0, 0, 0.03)
            border.width: 1
            border.color: FluTheme.dividerColor

            RowLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 10

                FluIcon {
                    iconSource: FluentIcons.Info
                    iconSize: 16
                    iconColor: FluTheme.fontSecondaryColor
                    Layout.alignment: Qt.AlignTop
                }

                FluText {
                    id: notice
                    Layout.fillWidth: true
                    text: App.platform === "小飞机网盘"
                          ? "点击“开始发布”后才会在本机流式加密，并逐片上传到小飞机网盘；每片会创建无提取码的永久分享，完成后登记取件码。"
                          : "点击“开始发布”后才会在本机生成载体并上传到所选平台；是否发布帖子或动态由账号页开关控制，上传成功后会登记取件码。"
                    font: FluTextStyle.Caption
                    textColor: FluTheme.fontSecondaryColor
                    wrapMode: Text.WordWrap
                }
            }
        }

        // 发放进度：阶段名 + 进度环，任何一步都能看出停在哪
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: progressColumn.implicitHeight + 24
            radius: 8
            visible: App.busy || App.stage.length > 0
            color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.05) : Qt.rgba(0, 0, 0, 0.03)
            border.width: 1
            border.color: FluTheme.dividerColor

            ColumnLayout {
                id: progressColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: 12
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    FluProgressRing {
                        Layout.preferredWidth: 18
                        Layout.preferredHeight: 18
                        visible: App.busy
                    }

                    FluText {
                        Layout.fillWidth: true
                        text: App.stage.length > 0 ? App.stage : "准备中…"
                        font: FluTextStyle.Body
                        elide: Text.ElideRight
                    }

                    FluText {
                        text: App.uploadSummary
                        font: FluTextStyle.Caption
                        textColor: FluTheme.primaryColor
                        visible: App.uploadSummary.length > 0
                    }
                }

                // 上传百分比：多张并发时按字节汇总，能看出"卡在哪、走了多少"
                FluProgressBar {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 6
                    visible: App.busy && App.uploadSummary.length > 0
                    indeterminate: false
                    strokeWidth: 6
                    value: Math.max(0, Math.min(100, App.progress)) / 100
                    Accessible.name: App.uploadSummary
                    Behavior on value {
                        NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
                    }
                }

                Repeater {
                    model: {
                        const stage = App.stage
                        const shouldPublish = App.publishToChannel
                        const platform = App.platform
                        return App.publishSteps()
                    }
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        FluText {
                            text: modelData.done ? "✓" : (modelData.active ? "…" : "○")
                            font: FluTextStyle.Caption
                            textColor: modelData.done ? FluTheme.primaryColor
                                       : modelData.active ? FluTheme.fontPrimaryColor
                                                          : FluTheme.fontTertiaryColor
                        }
                        FluText {
                            Layout.fillWidth: true
                            text: modelData.label
                            font: FluTextStyle.Caption
                            textColor: modelData.active ? FluTheme.fontPrimaryColor
                                                        : FluTheme.fontTertiaryColor
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 6
            spacing: 10

            FluButton {
                text: "返回"
                Layout.preferredWidth: 90
                Layout.preferredHeight: 36
                onClicked: Router.go("make")
            }

            FluFilledButton {
                text: App.busy ? "正在发布…" : "开始发布"
                disabled: App.busy
                Layout.preferredWidth: 150
                Layout.preferredHeight: 36
                onClicked: App.startMake()
            }

            FluText {
                text: "口令码永久有效"
                font: FluTextStyle.Caption
                textColor: FluTheme.fontTertiaryColor
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
            }
        }
    }
}
