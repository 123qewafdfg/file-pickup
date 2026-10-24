import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import FluentUI
import Cangjian

// 第三步：直接给取件码（口令）
PageScroller {
    id: page

    pageContent: ColumnLayout {
        Layout.fillWidth: true
        spacing: 18

        // ---------- 标题 ----------
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
                    iconSource: FluentIcons.Completed
                    iconSize: 20
                    iconColor: FluTheme.dark ? "#000000" : "#FFFFFF"
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3

                FluText {
                    text: App.code.length === 5 ? "已发放，这里是取件码" : "正在发放…"
                    font: FluTextStyle.TitleLarge
                }
                FluText {
                    text: App.code.length === 5
                          ? "对方无需登录，输入口令即可取回内容"
                          : App.stage.length > 0 ? App.stage : "正在处理，请稍候"
                    font: FluTextStyle.Body
                    textColor: FluTheme.fontSecondaryColor
                }
            }
        }

        // ---------- 口令卡 ----------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: codeColumn.implicitHeight + 48
            radius: 12
            color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.05) : Qt.rgba(0, 0, 0, 0.025)
            border.width: 1
            border.color: FluTheme.dividerColor

            ColumnLayout {
                id: codeColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: 24
                spacing: 16

                RowLayout {
                    Layout.fillWidth: true
                    FluText {
                        text: "取件码"
                        font: FluTextStyle.Caption
                        textColor: FluTheme.fontTertiaryColor
                    }
                    Item { Layout.fillWidth: true }
                    FluText {
                        text: "永久有效"
                        font: FluTextStyle.Caption
                        textColor: FluTheme.fontTertiaryColor
                    }
                }

                FluText {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignHCenter
                    text: App.code.length === 5
                          ? App.code.split("").join(" ")
                          : "· · · · ·"
                    font: FluTextStyle.Display
                    textColor: App.code.length === 5 ? FluTheme.fontPrimaryColor : FluTheme.fontTertiaryColor
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 10

                    FluFilledButton {
                        text: "复制口令"
                        disabled: App.code.length !== 5
                        Layout.preferredWidth: 132
                        Layout.preferredHeight: 36
                        onClicked: App.copyText(App.code)
                    }

                    FluButton {
                        text: "打开日志"
                        Layout.preferredWidth: 112
                        Layout.preferredHeight: 36
                        onClicked: App.openLog()
                    }

                    FluButton {
                        text: "再制作一份"
                        Layout.preferredWidth: 124
                        Layout.preferredHeight: 36
                        onClicked: {
                            App.resetMake()
                            App.resetExtract()
                            Router.go("make")
                        }
                    }
                }
            }
        }

        // ---------- 明细 ----------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: detail.implicitHeight + 28
            radius: 8
            color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.03) : Qt.rgba(1, 1, 1, 0.6)
            border.width: 1
            border.color: FluTheme.dividerColor

            ColumnLayout {
                id: detail
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: 14
                spacing: 8

                FluText {
                    text: "发放明细"
                    font: FluTextStyle.Caption
                    textColor: FluTheme.fontTertiaryColor
                }

                Repeater {
                    model: [
                        { label: "平台", value: App.platform },
                        { label: App.platform === "小飞机网盘" ? "加密分片" : "载体张数", value: String(App.carriers.length) },
                        { label: App.platform === "小飞机网盘" ? "分享方式" : "是否发布",
                          value: App.platform === "小飞机网盘" ? "永久分享（无提取码）"
                                                               : (App.publishToChannel ? "已发布到平台" : "未发布（已上传并登记直链）") },
                        { label: "日志文件", value: App.logPath() }
                    ]
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        FluText {
                            Layout.preferredWidth: 88
                            text: modelData.label
                            font: FluTextStyle.Caption
                            textColor: FluTheme.fontTertiaryColor
                        }
                        FluText {
                            Layout.fillWidth: true
                            text: modelData.value
                            font: FluTextStyle.Caption
                            elide: Text.ElideMiddle
                        }
                    }
                }
            }
        }

        FluText {
            Layout.fillWidth: true
            visible: App.platform !== "小飞机网盘" && !App.publishToChannel
            text: "本次未发布帖子或动态；取件码已登记上传直链，但平台是否长期保留孤立文件无法由客户端保证。"
            font: FluTextStyle.Caption
            textColor: "#C42B1C"
            wrapMode: Text.WordWrap
        }
    }
}
