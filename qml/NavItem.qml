import QtQuick
import QtQuick.Controls
import FluentUI

// 左侧导航项：选中时左侧显示 3px 强调条
Button {
    id: control

    property int iconSource
    property bool selected: false

    implicitHeight: 40
    focusPolicy: Qt.NoFocus

    background: Rectangle {
        radius: 6
        color: control.selected
               ? (control.hovered ? FluTheme.itemCheckColor : FluTheme.itemHoverColor)
               : (control.hovered ? FluTheme.itemHoverColor : "transparent")

        Rectangle {
            width: 3
            height: 16
            radius: 1.5
            color: FluTheme.primaryColor
            visible: control.selected
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    contentItem: Item {
        implicitHeight: 40

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            spacing: 12

            FluIcon {
                iconSource: control.iconSource
                iconSize: 16
                iconColor: control.selected ? FluTheme.primaryColor : FluTheme.fontPrimaryColor
                anchors.verticalCenter: parent.verticalCenter
            }

            FluText {
                text: control.text
                font: control.selected ? FluTextStyle.BodyStrong : FluTextStyle.Body
                textColor: FluTheme.fontPrimaryColor
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }
}
