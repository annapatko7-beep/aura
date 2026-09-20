cmake_minimum_required(VERSION 3.21)

project(aura-client VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Клиент использует QtWebSockets и MultiEffect (QtQuick.Effects) → нужен Qt 6.5+.
# Multimedia — захват микрофона (STT), TextToSpeech — платформенная озвучка (TTS).
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 WebSockets Multimedia TextToSpeech)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(aura-client
    src/main.cpp
    src/appstore.cpp
    src/appstore.h
    src/websocketclient.cpp
    src/websocketclient.h
)

# Все QML-файлы живут в одном модуле Aura: внутри них достаточно `import Aura`.
set_source_files_properties(qml/theme/AuraTheme.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(aura-client
    URI Aura
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    QML_FILES
        qml/Main.qml
        qml/LoginPage.qml
        qml/ChatsPage.qml
        qml/ChatPage.qml
        qml/SettingsPage.qml
        qml/DesktopShell.qml
        qml/theme/AuraTheme.qml
        qml/components/GlassSurface.qml
        qml/components/GlassPanel.qml
        qml/components/GlassButton.qml
        qml/components/GlassField.qml
        qml/components/ChatBubble.qml
        qml/components/AuraAvatar.qml
        qml/components/VoicePanel.qml
        qml/components/ConfirmationPanel.qml
        qml/components/TasksPanel.qml
        qml/components/ChatList.qml
)

target_link_libraries(aura-client PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::WebSockets
    Qt6::Multimedia
    Qt6::TextToSpeech
)

target_compile_options(aura-client PRIVATE -Wall -Wextra)

install(TARGETS aura-client RUNTIME DESTINATION bin)
