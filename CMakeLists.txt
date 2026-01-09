cmake_minimum_required(VERSION 3.16)
project(ringlight VERSION 1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# Find LayerShellQt (required for proper Wayland overlay)
find_package(LayerShellQt REQUIRED)

# === Overlay ===
add_executable(ringlight-overlay src/overlay.cpp)
target_link_libraries(ringlight-overlay PRIVATE
    Qt6::Core
    Qt6::Gui
    LayerShellQt::Interface
)

# === GUI Application ===
add_executable(ringlight-gui src/gui.cpp)
target_link_libraries(ringlight-gui PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

# === Monitor Daemon (pure C) ===
add_executable(ringlight-monitor src/monitor.c)

# === Installation ===
include(GNUInstallDirs)

install(TARGETS ringlight-overlay ringlight-gui ringlight-monitor
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES systemd/ringlight-monitor.service
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user
)

install(FILES resources/ringlight.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
)
