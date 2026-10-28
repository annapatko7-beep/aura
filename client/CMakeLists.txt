cmake_minimum_required(VERSION 3.21)

project(selfvoy-client VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Клиент использует QtWebSockets, QtCore.Settings и Qt.styleHints.colorScheme (тема по системе) → нужен Qt 6.5+.
# Multimedia — захват микрофона (STT), TextToSpeech — платформенная озвучка (TTS).
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 WebSockets Multimedia TextToSpeech)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(selfvoy-client
    src/main.cpp
    src/appstore.cpp
    src/appstore.h
    src/websocketclient.cpp
    src/websocketclient.h
)

# Все QML-файлы живут в одном модуле Selfvoy: внутри них достаточно `import Selfvoy`.
set_source_files_properties(qml/theme/SelfvoyTheme.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(selfvoy-client
    URI Selfvoy
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    QML_FILES
        qml/Main.qml
        qml/LoginPage.qml
        qml/OnboardingPage.qml
        qml/DesktopShell.qml
        qml/HomePage.qml
        qml/MySelfvoyPage.qml
        qml/NegotiationsPage.qml
        qml/TasksPage.qml
        qml/ContactsPage.qml
        qml/ChatPage.qml
        qml/SettingsPage.qml
        qml/InboxPage.qml
        qml/BookingPage.qml
        qml/PackagesPage.qml
        qml/Format.js
        qml/icons/Icons.js
        qml/theme/SelfvoyTheme.qml
        qml/components/SelfvoyBackground.qml
        qml/components/GlassSurface.qml
        qml/components/GlassPanel.qml
        qml/components/GlassButton.qml
        qml/components/GlassField.qml
        qml/components/GlassComboBox.qml
        qml/components/Icon.qml
        qml/components/LinkText.qml
        qml/components/Chip.qml
        qml/components/Bubble.qml
        qml/components/AgentCard.qml
        qml/components/SelfvoyEmblem.qml
        qml/components/SelfvoyOrb.qml
        qml/components/SelfvoyAvatar.qml
        qml/components/ChatBubble.qml
        qml/components/VoicePanel.qml
        qml/components/ConfirmationPanel.qml
        qml/components/NotificationsPanel.qml
        qml/components/SelfvoyLogo.qml
        qml/components/StatusPill.qml
        qml/components/ProBadge.qml
        qml/components/Segmented.qml
        qml/components/Toggle.qml
        qml/components/Group.qml
        qml/components/Cell.qml
        qml/components/SheetPopup.qml
        qml/components/BookingDetailPanel.qml
)

# Manrope (SIL OFL 1.1, fonts/OFL.txt) — шрифт макетов, встроен в клиент.
qt_add_resources(selfvoy-client "fonts"
    PREFIX "/"
    FILES
        fonts/Manrope-Regular.ttf
        fonts/Manrope-Medium.ttf
        fonts/Manrope-SemiBold.ttf
        fonts/Manrope-Bold.ttf
        fonts/Manrope-ExtraBold.ttf
)

# Иконка окна (Linux/macOS — через setWindowIcon) из того же арта, что и у мобильных.
qt_add_resources(selfvoy-client "icons"
    PREFIX "/"
    FILES
        icons/selfvoy.png
)

# Windows: иконка exe, ярлыков и панели задач.
if(WIN32)
    target_sources(selfvoy-client PRIVATE packaging/windows/selfvoy.rc)
endif()

target_link_libraries(selfvoy-client PRIVATE
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

# Windows: GUI-приложение без консольного окна.
set_target_properties(selfvoy-client PROPERTIES WIN32_EXECUTABLE ON)

if(NOT MSVC)
    target_compile_options(selfvoy-client PRIVATE -Wall -Wextra)
endif()

install(TARGETS selfvoy-client RUNTIME DESTINATION bin)
