cmake_minimum_required(VERSION 3.21)

project(dvoice-qt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Multimedia Svg)

add_library(dvoice-core STATIC
    src/core/asr.cpp
    src/core/tts.cpp
    src/core/recorder.cpp
    src/core/player.cpp
    src/core/settings.cpp
    src/core/wav.cpp
    src/core/audiodevices.cpp
)
target_include_directories(dvoice-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(dvoice-core PUBLIC DV_VERSION="${PROJECT_VERSION}")
target_link_libraries(dvoice-core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Multimedia
)

add_executable(dvoice-qt
    src/main.cpp
    src/theme.cpp
    src/audiomonitor.cpp
    src/preferences.cpp
    src/devicetest.cpp
    src/mainwindow.cpp
    resources.qrc
)
target_link_libraries(dvoice-qt PRIVATE dvoice-core Qt6::Widgets Qt6::Svg)

install(TARGETS dvoice-qt RUNTIME DESTINATION bin)
install(FILES assets/app.svg DESTINATION share/icons/hicolor/scalable/apps RENAME dvoice-qt.svg)
install(FILES assets/dvoice-qt.desktop DESTINATION share/applications)
