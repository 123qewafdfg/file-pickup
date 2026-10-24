import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import FluentUI
import Cangjian

PageScroller {
    id: page

    pageContent: ColumnLayout {
        Layout.fillWidth: true
        spacing: 16

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6
            FluText {
                text: "账号管理"
                font: FluTextStyle.TitleLarge
            }
            FluText {
                text: "每个平台独占一行，点击进入详情查看发放次数与长期直链接。"
                font: FluTextStyle.Body
                textColor: FluTheme.fontSecondaryColor
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        Repeater {
            model: App.accounts
            delegate: Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 76
                radius: 8
                color: rowMouse.containsMouse
                       ? (FluTheme.dark ? Qt.rgba(1, 1, 1, 0.055) : Qt.rgba(0, 0, 0, 0.025))
                       : "transparent"
                border.width: 1
                border.color: FluTheme.dividerColor

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

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        RowLayout {
                            spacing: 8
                            FluText {
                                text: modelData.name
                                font: FluTextStyle.BodyStrong
                            }
                            FluText {
                                visible: modelData.recommended
                                text: "推荐"
                                font: FluTextStyle.Caption
                                textColor: FluTheme.primaryColor
                            }
                        }

                        FluText {
                            Layout.fillWidth: true
                            text: modelData.connected
                                  ? "已连接 · 累计发放 " + modelData.count + " 次"
                                  : "未登录 · " + modelData.detail
                            font: FluTextStyle.Caption
                            textColor: FluTheme.fontTertiaryColor
                            elide: Text.ElideRight
                        }
                    }

                    FluButton {
                        text: modelData.connected ? "退出" : "登录"
                        Layout.preferredWidth: 80
                        Layout.preferredHeight: 36
                        onClicked: {
                            if (modelData.connected)
                                App.logoutPlatform(modelData.name)
                            else {
                                Router.platformName = modelData.name
                                Router.go("login")
                            }
                        }
                    }

                    FluIconButton {
                        iconSource: FluentIcons.ChevronRight
                        iconSize: 14
                        implicitWidth: 36
                        implicitHeight: 36
                        ToolTip.text: "查看" + modelData.name + "详情"
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: {
                            Router.platformName = modelData.name
                            Router.go("detail")
                        }
                    }
                }

                // 只负责悬停高亮；点击交给行内按钮，避免吞掉“登录”
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.NoButton
                }
            }
        }

    }
}
