import QtQuick
import Selfvoy

/*
  SelfvoyLogo — знак Selfvoy: S-линия между двумя точками. Линия — акцент
  #3555F5, верхняя точка — цвет текста темы, нижняя — янтарная #FFB224
  (как Logo в web/src/components/ui.tsx). mono — всё одним цветом.
*/
Item {
    id: root
    property int size: 26
    property bool mono: false
    property color monoColor: SelfvoyTheme.text

    implicitWidth: size
    implicitHeight: size

    readonly property string markup: {
        const line = mono ? monoColor.toString() : "#3555F5"
        const top = mono ? monoColor.toString() : SelfvoyTheme.text.toString()
        const bottom = mono ? monoColor.toString() : "#FFB224"
        return '<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100" fill="none">'
            + '<path d="M76 28 C 38 24, 28 44, 50 50 C 72 56, 62 76, 24 72" stroke="' + line
            + '" stroke-width="11" stroke-linecap="round"/>'
            + '<circle cx="76" cy="28" r="10" fill="' + top + '"/>'
            + '<circle cx="24" cy="72" r="10" fill="' + bottom + '"/></svg>'
    }

    Image {
        anchors.fill: parent
        sourceSize: Qt.size(root.size * 2, root.size * 2)
        smooth: true
        source: "data:image/svg+xml;utf8," + encodeURIComponent(root.markup)
    }
}
