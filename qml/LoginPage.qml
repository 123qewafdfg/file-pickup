import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import FluentUI
import Cangjian

PageScroller {
    id: page

    property string platform: "QQ 频道"
    readonly property bool isBili: platform === "哔哩哔哩"
    readonly property bool isFeijii: platform === "小飞机网盘"

    // 进入页面自动获取二维码
    Component.onCompleted: {
        if (isFeijii) {
            if (!App.feijiiConnected && App.feijiiState === "idle")
                App.startFeijiiLogin()
        } else if (isBili) {
            // 页面可能被创建两次，只让第一次发起扫码
            if (!App.biliConnected && App.biliState === "idle")
                App.startBiliLogin()
        } else if (App.qqLogin && App.qqLogin.state === "idle" && !App.qqLogin.loggedIn) {
            App.qqLogin.start()
        }
    }

    function stateText() {
        if (!App.qqLogin)
            return ""
        switch (App.qqLogin.state) {
        case "loading": return "正在获取二维码…"
        case "waiting": return "请用手机 QQ 扫描下方二维码"
        case "scanned": return "已扫描，请在手机上确认登录"
        case "confirmed": return "已确认，正在完成登录…"
        case "expired": return "二维码已失效，点“刷新二维码”重来"
        case "loggedIn": return "QQ 已登录"
        case "error": return App.qqLogin.message
        default: return "点击“获取二维码”开始登录"
        }
    }

    pageContent: ColumnLayout {
        Layout.fillWidth: true
        spacing: 18

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6

            FluText {
                text: page.isFeijii ? "小飞机网盘登录"
                                    : page.isBili ? "哔哩哔哩登录" : "QQ 频道登录"
                font: FluTextStyle.TitleLarge
            }
            FluText {
                visible: !page.isBili && !page.isFeijii
                Layout.fillWidth: true
                text: "用手机 QQ 扫码即可登录，登录状态保存在本机，不经过任何第三方。"
                font: FluTextStyle.Body
                textColor: FluTheme.fontSecondaryColor
                wrapMode: Text.WordWrap
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 420
            visible: !page.isBili && !page.isFeijii
            radius: 8
            color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.045) : Qt.rgba(1, 1, 1, 0.7)
            border.width: 1
            border.color: FluTheme.dividerColor

            ColumnLayout {
                anchors.centerIn: parent
                spacing: 14

                Rectangle {
                    Layout.preferredWidth: 260
                    Layout.preferredHeight: 260
                    Layout.alignment: Qt.AlignHCenter
                    radius: 8
                    color: "#FFFFFF"
                    border.width: 1
                    border.color: FluTheme.dividerColor

                    Image {
                        anchors.fill: parent
                        anchors.margins: 8
                        visible: App.qqLogin && App.qqLogin.qrImagePath.length > 0
                        source: App.qqLogin && App.qqLogin.qrImagePath.length > 0
                                ? "file:///" + App.qqLogin.qrImagePath : ""
                        fillMode: Image.PreserveAspectFit
                        cache: false
                        asynchronous: true
                    }

                    FluIcon {
                        anchors.centerIn: parent
                        visible: !App.qqLogin || App.qqLogin.qrImagePath.length === 0
                        iconSource: FluentIcons.QRCode
                        iconSize: 56
                        iconColor: "#9AA0A6"
                    }
                }

                FluText {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.maximumWidth: 420
                    text: page.stateText()
                    font: FluTextStyle.Body
                    textColor: FluTheme.fontSecondaryColor
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }
            }
        }

        // B站和小飞机均由登录脚本生成二维码，界面复用同一块布局。
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: biliColumn.implicitHeight + 32
            visible: page.isBili || page.isFeijii
            radius: 8
            color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.045) : Qt.rgba(1, 1, 1, 0.7)
            border.width: 1
            border.color: FluTheme.dividerColor

            ColumnLayout {
                id: biliColumn
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                Rectangle {
                    Layout.preferredWidth: 220
                    Layout.preferredHeight: 220
                    Layout.alignment: Qt.AlignHCenter
                    radius: 8
                    color: "#FFFFFF"
                    border.width: 1
                    border.color: FluTheme.dividerColor

                    Image {
                        anchors.fill: parent
                        anchors.margins: 8
                        property string qrPath: page.isFeijii ? App.feijiiQrPath : App.biliQrPath
                        visible: qrPath.length > 0
                        source: qrPath.length > 0 ? "file:///" + qrPath : ""
                        fillMode: Image.PreserveAspectFit
                        cache: false
                        asynchronous: true
                    }

                    FluIcon {
                        anchors.centerIn: parent
                        visible: (page.isFeijii ? App.feijiiQrPath.length === 0 || App.feijiiState === "expired"
                                               : App.biliQrPath.length === 0 || App.biliState === "expired")
                        iconSource: FluentIcons.QRCode
                        iconSize: 48
                        iconColor: "#9AA0A6"
                    }
                }

                FluText {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.maximumWidth: 460
                    text: page.isFeijii
                          ? (App.feijiiMessage.length > 0 ? App.feijiiMessage : "点下方“刷新二维码”开始登录")
                          : (App.biliMessage.length > 0 ? App.biliMessage : "点下方“刷新二维码”开始登录")
                    font: FluTextStyle.Body
                    textColor: FluTheme.fontSecondaryColor
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 10

                    FluButton {
                        text: "刷新二维码"
                        Layout.preferredWidth: 120
                        Layout.preferredHeight: 36
                        onClicked: page.isFeijii ? App.startFeijiiLogin(true) : App.startBiliLogin(true)
                    }
                    FluButton {
                        text: "退出登录"
                        visible: page.isFeijii ? App.feijiiConnected : App.biliConnected
                        Layout.preferredWidth: 100
                        Layout.preferredHeight: 36
                        onClicked: page.isFeijii ? App.logoutPlatform("小飞机网盘") : App.clearBiliCookie()
                    }
                }
            }
        }

        // 是否发布作品：关闭时只上传不公开，直链为临时地址
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 64
            visible: !page.isBili && !page.isFeijii
            radius: 8
            color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.045) : Qt.rgba(1, 1, 1, 0.7)
            border.width: 1
            border.color: FluTheme.dividerColor

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                spacing: 12

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    FluText {
                        text: "上传后发布帖子/动态"
                        font: FluTextStyle.Body
                    }
                    FluText {
                        Layout.fillWidth: true
                        text: App.publishToChannel
                              ? "上传完成后发布到平台"
                              : "只上传并登记直链，不发布帖子或动态"
                        font: FluTextStyle.Caption
                        textColor: FluTheme.fontTertiaryColor
                        elide: Text.ElideRight
                    }
                }

                FluToggleSwitch {
                    checked: App.publishToChannel
                    onToggled: App.publishToChannel = checked
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: !page.isBili && !page.isFeijii
            spacing: 10

            FluFilledButton {
                text: App.qqLogin && App.qqLogin.state === "loggedIn" ? "已登录" : "获取二维码"
                disabled: App.qqLogin && (App.qqLogin.busy || App.qqLogin.state === "loggedIn")
                Layout.preferredWidth: 128
                Layout.preferredHeight: 36
                onClicked: App.qqLogin.start()
            }

            FluButton {
                text: "刷新二维码"
                Layout.preferredWidth: 116
                Layout.preferredHeight: 36
                onClicked: App.qqLogin.refresh()
            }

            FluButton {
                text: "返回账号管理"
                Layout.preferredWidth: 128
                Layout.preferredHeight: 36
                onClicked: Router.go("accounts")
            }
        }
    }
}
