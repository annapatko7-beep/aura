import QtQuick
import Selfvoy

/*
  GlassPanel — карточка 2026 (как .card в вебе): сплошная поверхность, радиус 20,
  внутренние отступы. Старые свойства (backdrop, glow, fill, tint…) принимаются,
  но стекла и свечения нет: glow лишь подкрашивает рамку акцентом.
*/
Item {
    id: root

    property alias backdrop: surface.backdrop
    property alias radius: surface.radius
    property alias edge: surface.edge
    property alias base: surface.base
    property real fill: SelfvoyTheme.glassFill
    property color tint: SelfvoyTheme.surface
    property alias color: surface.color
    property bool hovered: false
    property int padding: SelfvoyTheme.spaceLg - 6
    property bool glow: false
    property color glowColor: SelfvoyTheme.accent
    property bool shadow: false
    // data, а не children: внутрь панели кладут и невизуальные объекты (Connections, Timer).
    default property alias content: holder.data

    implicitWidth: contentImplicit("implicitWidth") + padding * 2
    implicitHeight: contentImplicit("implicitHeight") + padding * 2

    function contentImplicit(prop) {
        let size = 0
        for (let i = 0; i < holder.children.length; ++i) {
            const child = holder.children[i]
            if (child.visible) size = Math.max(size, child[prop])
        }
        return size
    }

    GlassSurface {
        id: surface
        anchors.fill: parent
        radius: SelfvoyTheme.radiusCard
        hovered: root.hovered

        Item {
            id: holder
            anchors.fill: parent
            anchors.margins: root.padding
        }
    }
    Rectangle {
        visible: root.glow
        anchors.fill: parent
        radius: surface.radius
        color: "transparent"
        border.width: 1.5
        border.color: root.glowColor
    }
}
