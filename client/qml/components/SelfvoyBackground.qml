import QtQuick
import Selfvoy

/*
  SelfvoyBackground — фон окна: сплошной цвет темы (светлый #F2F2F7 или чёрный).
  Орбиты и градиенты прежнего «стеклянного» стиля убраны: текст всегда лежит
  на сплошной поверхности. animated оставлен для совместимости.
*/
Rectangle {
    id: root
    property bool animated: false
    color: SelfvoyTheme.bg
}
