pragma Singleton

import QtQuick

// 页面路由：Main.qml 用 Loader 绑定 route，页面内部调用 go() 跳转
QtObject {
    id: router

    property string route: "make"
    property string platformName: "小飞机网盘"
    // 仅在当前运行期间记住制作模块所处步骤；应用重启后仍从制作首页开始。
    property string lastMakeRoute: "make"

    function go(name) {
        if (name && name.length > 0) {
            if (name === "make" || name === "platform" || name === "done")
                router.lastMakeRoute = name
            router.route = name
        }
    }

    function goMake() {
        router.route = router.lastMakeRoute
    }

    function isMain() {
        return route === "make" || route === "extract"
    }
}
