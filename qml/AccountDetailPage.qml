import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import FluentUI
import Cangjian

PageScroller {
    id: page

    property string platform: "QQ 频道"

    function account() {
        for (var i = 0; i < App.accounts.length; ++i) {
            if (App.accounts[i].name === page.platform)
                return App.accounts[i]
        }
        return ({ name: page.platform, connected: false, count: 0, detail: "" })
    }

    pageContent: ColumnLayout {
        Layout.fillWidth: true
        spacing: 16

        RowLayout {
            Layout.fillWidth: true
            spacing: 14

            PlatformLogo {
                Layout.preferredWidth: 48
                Layout.preferredHeight: 48
                platform: page.platform
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                FluText {
                    text: page.platform
                    font: FluTextStyle.TitleLarge
                }
                FluText {
                    text: page.account().connected ? "已连接" : "未登录"
                    font: FluTextStyle.Caption
                    textColor: FluTheme.fontTertiaryColor
                }
            }

            FluButton {
                text: page.account().connected ? "退出登录" : "登录"
                Layout.preferredWidth: 96
                Layout.preferredHeight: 36
                onClicked: {
                    if (page.account().connected)
                        App.logoutPlatform(page.platform)
                    else {
                        Router.platformName = page.platform
                        Router.go("login")
                    }
                }
            }

            FluTextButton {
                text: "返回账号管理"
                onClicked: Router.go("accounts")
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: overview.implicitHeight + 32
            radius: 8
            color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.045) : Qt.rgba(1, 1, 1, 0.7)
            border.width: 1
            border.color: FluTheme.dividerColor

            ColumnLayout {
                id: overview
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: 18
                spacing: 10

                FluText {
                    text: "账户概览"
                    font: FluTextStyle.Caption
                    textColor: FluTheme.fontTertiaryColor
                }

                Repeater {
                    model: page.platform === "哔哩哔哩"
                           ? [{ icon: FluentIcons.Certificate, label: "账号等级", value: "未获取" },
                              { icon: FluentIcons.History, label: "累计发放", value: page.account().count + " 次" },
                              { icon: FluentIcons.Lock, label: "登录凭据", value: "仅保存在本机" }]
                           : [{ icon: FluentIcons.History, label: "累计发放", value: page.account().count + " 次" },
                              { icon: FluentIcons.Lock, label: "登录凭据", value: "仅保存在本机" }]
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        FluIcon {
                            iconSource: modelData.icon
                            iconSize: 15
                            iconColor: FluTheme.fontSecondaryColor
                        }
                        FluText {
                            text: modelData.label
                            font: FluTextStyle.Body
                            Layout.preferredWidth: 90
                        }
                        FluText {
                            Layout.fillWidth: true
                            text: modelData.value
                            font: FluTextStyle.Body
                            textColor: FluTheme.fontSecondaryColor
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            visible: page.platform !== "小飞机网盘"
            radius: 8
            color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.045) : Qt.rgba(1, 1, 1, 0.7)
            border.width: 1
            border.color: FluTheme.dividerColor

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                spacing: 12

                FluIcon {
                    iconSource: FluentIcons.Link
                    iconSize: 16
                    iconColor: FluTheme.fontSecondaryColor
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    FluText {
                        text: "长期直链"
                        font: FluTextStyle.Body
                    }
                    FluText {
                        text: page.platform === "哔哩哔哩"
                              ? "优先使用平台长期图片地址"
                              : "登记审核后的频道图片地址"
                        font: FluTextStyle.Caption
                        textColor: FluTheme.fontTertiaryColor
                    }
                }

                FluToggleSwitch {
                    checked: App.longLink(page.platform)
                    onToggled: App.setLongLink(page.platform, checked)
                }
            }
        }

        FluText {
            Layout.fillWidth: true
            text: page.platform === "小飞机网盘"
                  ? "文件在本机使用 secretstream 流式加密，逐片上传并创建无提取码的永久分享；不会发布帖子或动态。"
                  : page.platform === "哔哩哔哩"
                  ? "关闭后仍可发放，当前版本仍使用平台返回的图片地址。"
                  : "QQ 频道固定使用频道 pd55235600；每条作品最多 18 张载体，超出后自动拆为多条。"
            font: FluTextStyle.Caption
            textColor: FluTheme.fontTertiaryColor
            wrapMode: Text.WordWrap
        }
    }
}
