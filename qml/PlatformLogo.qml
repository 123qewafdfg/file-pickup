import QtQuick

Rectangle {
    id: root

    property string platform: "QQ 频道"

    implicitWidth: 40
    implicitHeight: 40
    radius: 8
    color: "transparent"
    clip: true

    Image {
        anchors.fill: parent
        source: root.platform === "QQ 频道"
                ? "qrc:/icons/qq-channel.svg"
                : root.platform === "小飞机网盘"
                  ? "qrc:/icons/feijii.svg"
                  : "qrc:/icons/bilibili.png"
        fillMode: Image.PreserveAspectFit
        smooth: true
        mipmap: true
        asynchronous: true
    }

    Accessible.role: Accessible.Graphic
    Accessible.name: root.platform + "图标"
}
