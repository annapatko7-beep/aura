import QtQuick

/*
  GlassPanel — стеклянная карточка (списки, формы, диалог).
  Тонкая обёртка над GlassSurface с тенью и отступами.
*/
Item {
    id: root

    property alias backdrop: surface.backdrop
    property alias radius: surface.radius
    property real fill: AuraTheme.glassFill
    property bool hovered: false
    property int padding: AuraTheme.spaceLg
    property alias content: holder.data
    default property alias children_: holder.children

    implicitWidth: holder.implicitWidth + padding * 2
    implicitHeight: holder.implicitHeight + padding * 2

    // Мягкая тень под панелью
    Rectangle {
        anchors.fill: surface
        anchors.margins: -10
        radius: surface.radius + 10
        color: AuraTheme.shadowColor
        opacity: 0.45
    }

    GlassSurface {
        id: surface
        anchors.fill: parent
        radius: AuraTheme.radiusCard
        fill: root.fill
        hovered: root.hovered

        Item {
            id: holder
            anchors.fill: parent
            anchors.margins: root.padding
        }
    }
}
