import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import FluentUI

// 设置行：图标 + 标题 + 说明 + Fluent 开关
Item {
    id: root

    property int iconSource
    property string title: ""
    property string detail: ""
    property bool checked: false

    signal toggled(bool value)

    implicitHeight: 54
    height: 54

    Item {
        id: iconSlot
        width: 24
        height: 24
        anchors.left: parent.left
        anchors.leftMargin: 6
        anchors.verticalCenter: parent.verticalCenter

        FluIcon {
            anchors.centerIn: parent
            iconSource: root.iconSource
            iconSize: 17
            iconColor: FluTheme.fontSecondaryColor
        }
    }

    Column {
        anchors.left: iconSlot.right
        anchors.leftMargin: 12
        anchors.right: settingSwitch.left
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        height: childrenRect.height
        spacing: 1

        FluText {
            width: parent.width
            text: root.title
            font: FluTextStyle.Body
            elide: Text.ElideRight
        }
        FluText {
            width: parent.width
            text: root.detail
            font: FluTextStyle.Caption
            textColor: FluTheme.fontTertiaryColor
            elide: Text.ElideRight
            visible: root.detail.length > 0
        }
    }

    FluToggleSwitch {
        id: settingSwitch
        width: 44
        anchors.right: parent.right
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        checked: root.checked
        onToggled: root.toggled(checked)
    }
}
