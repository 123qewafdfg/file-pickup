import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import FluentUI
import Cangjian

FluWindow {
    id: window

    // 逻辑尺寸受当前屏幕可用区域约束（本机 150% 缩放，必须夹住）
    width: Math.min(App.windowWidth, Screen.desktopAvailableWidth - 60)
    height: Math.min(App.windowHeight, Screen.desktopAvailableHeight - 60)
    minimumWidth: 860
    minimumHeight: 560
    visible: true
    title: "文件取件"

    function applyNextTheme() {
        const targetDark = !FluTheme.dark
        App.darkMode = targetDark
        FluTheme.darkMode = targetDark ? FluThemeType.Dark : FluThemeType.Light
    }

    function handleThemeChanged(button) {
        applyNextTheme()
    }

    onClosing: function(close) {
        App.saveWindowSize(width, height)
    }

    // Windows 11 风格标题栏（自带深浅色切换）
    appBar: FluAppBar {
        title: "文件取件"
        showDark: true
        showStayTop: false
        darkClickListener: function(button) { window.handleThemeChanged(button) }
    }

    FluInfoBar {
        id: infoBar
        root: window
    }

    Dialog {
        id: updateDialog
        anchors.centerIn: parent
        width: Math.min(480, window.width - 80)
        modal: true
        title: "发现新版本 " + App.updateVersion

        contentItem: ColumnLayout {
            spacing: 10

            FluText {
                Layout.fillWidth: true
                text: "安装包大小：" + App.updateSizeText
                font: FluTextStyle.Body
            }

            FluText {
                Layout.fillWidth: true
                text: App.updateNotes
                font: FluTextStyle.Body
                wrapMode: Text.WordWrap
            }
        }

        footer: DialogButtonBox {
            Button {
                text: "下载安装"
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            Button {
                text: "稍后"
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }

        onAccepted: App.openUpdateDownload()
    }

    // 联调钩子：CJ_ROUTE 直接进指定页面，CJ_DEMO=1 自动跑一遍制作流程，
    // CJ_SHOT=<路径> 抓一张 PNG 后自动退出（用于验收截图，避免窗管/DPI 干扰）
    Component.onCompleted: {
        FluTheme.darkMode = App.darkMode ? FluThemeType.Dark : FluThemeType.Light
        if (App.initialRoute.length > 0)
            Router.go(App.initialRoute)
        if (App.demoMode) {
            App.demoFill()
            Router.go("platform")
            Qt.callLater(function() { App.startMake() })
        }
        if (App.screenshotPath.length > 0)
            shotTimer.start()
    }

    Timer {
        id: shotTimer
        interval: 3000
        repeat: false
        onTriggered: {
            window.contentItem.grabToImage(function(result) {
                result.saveToFile(App.screenshotPath)
                Qt.quit()
            }, Qt.size(window.width, window.height))
        }
    }

    Connections {
        target: App
        function onNotify(message) { infoBar.showInfo(message) }
        function onUpdateReady() { updateDialog.open() }
        function onSettingsChanged() {
            const targetMode = App.darkMode ? FluThemeType.Dark : FluThemeType.Light
            if (FluTheme.darkMode !== targetMode)
                FluTheme.darkMode = targetMode
        }
        function onOutputsChanged() {
            if (App.outputs.length > 0 && Router.route === "extract")
                Router.go("extractResult")
        }
        function onRequestRoute(route) {
            if (route === "login")
                Router.platformName = App.platform
            Router.go(route)
        }
        function onCodeChanged() {
            if (App.code.length === 5 && (Router.route === "platform" || Router.route === "make"))
                Router.go("done")
        }
    }


    RowLayout {
        id: contentRoot
        anchors.fill: parent
        spacing: 0

        // ---------------- 左侧导航 ----------------
        Item {
            Layout.preferredWidth: 220
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 2

                Item { Layout.preferredHeight: 6 }

                Repeater {
                    model: [
                        { label: "制作", icon: FluentIcons.Send, route: "make" },
                        { label: "提取", icon: FluentIcons.Link, route: "extract" }
                    ]
                    delegate: NavItem {
                        Layout.fillWidth: true
                        text: modelData.label
                        iconSource: modelData.icon
                        selected: Router.route === modelData.route
                                  || (modelData.route === "make"
                                      && (Router.route === "platform" || Router.route === "done"))
                                  || (modelData.route === "extract"
                                      && Router.route === "extractResult")
                        onClicked: {
                            if (modelData.route === "make")
                                Router.goMake()
                            else
                                Router.go(modelData.route)
                        }
                    }
                }

                Item { Layout.preferredHeight: 16 }

                FluText {
                    text: "记录与账号"
                    font: FluTextStyle.Caption
                    textColor: FluTheme.fontTertiaryColor
                    Layout.leftMargin: 16
                    Layout.bottomMargin: 4
                }

                NavItem {
                    Layout.fillWidth: true
                    text: "历史记录"
                    iconSource: FluentIcons.History
                    selected: Router.route === "history"
                    onClicked: Router.go("history")
                }

                NavItem {
                    Layout.fillWidth: true
                    text: "账号管理"
                    iconSource: FluentIcons.Contact
                    selected: Router.route === "accounts" || Router.route === "detail"
                              || Router.route === "login"
                    onClicked: Router.go("accounts")
                }

                Item { Layout.fillHeight: true }

                NavItem {
                    Layout.fillWidth: true
                    text: App.updateChecking ? "正在检查…" : "检查更新"
                    iconSource: FluentIcons.Sync
                    enabled: !App.updateChecking
                    selected: false
                    onClicked: App.checkForUpdates()
                }

                FluText {
                    text: "口令码永久有效"
                    font: FluTextStyle.Caption
                    textColor: FluTheme.fontTertiaryColor
                    Layout.leftMargin: 16
                    Layout.bottomMargin: 6
                }
            }
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            color: FluTheme.dividerColor
        }

        // ---------------- 右侧内容 ----------------
        StackLayout {
            id: pageStack
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0

            function indexForRoute(route) {
                switch (route) {
                case "platform": return 1
                case "done": return 2
                case "extract": return 3
                case "extractResult": return 4
                case "history": return 5
                case "accounts": return 6
                case "detail": return 7
                case "login": return 8
                default: return 0
                }
            }

            currentIndex: indexForRoute(Router.route)

            // 制作流程、提取输入和历史滚动位置保持；结果与账号子页按需卸载，
            // 避免不可见页面继续运行 bindings 和占用 GUI 线程。
            Loader { active: Router.route === "make" || item !== null; sourceComponent: makePageComponent }
            Loader { active: Router.route === "platform" || item !== null; sourceComponent: platformPageComponent }
            Loader { active: Router.route === "done"; asynchronous: true; sourceComponent: donePageComponent }
            Loader { active: Router.route === "extract" || item !== null; sourceComponent: extractPageComponent }
            Loader { active: Router.route === "extractResult"; asynchronous: true; sourceComponent: extractResultComponent }
            Loader { active: Router.route === "history" || item !== null; sourceComponent: historyPageComponent }
            Loader { active: Router.route === "accounts"; asynchronous: true; sourceComponent: accountsPageComponent }
            Loader { active: Router.route === "detail"; asynchronous: true; sourceComponent: detailPageComponent }
            Loader { active: Router.route === "login"; asynchronous: true; sourceComponent: loginPageComponent }
        }
    }

    // ---------------- 页面组件 ----------------
    Component {
        id: makePageComponent
        MakePage {}
    }
    Component {
        id: platformPageComponent
        PlatformPage {}
    }
    Component {
        id: donePageComponent
        DonePage {}
    }
    Component {
        id: extractPageComponent
        ExtractPage {}
    }
    Component {
        id: extractResultComponent
        ExtractResultPage {}
    }
    Component {
        id: historyPageComponent
        HistoryPage {}
    }
    Component {
        id: accountsPageComponent
        AccountsPage {}
    }
    Component {
        id: detailPageComponent
        AccountDetailPage { platform: Router.platformName }
    }
    Component {
        id: loginPageComponent
        LoginPage { platform: Router.platformName }
    }
}
