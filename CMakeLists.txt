cmake_minimum_required(VERSION 3.16)
project(ringlight VERSION 1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Gui)

# Find LayerShellQt
find_package(LayerShellQt REQUIRED)

# RingLight (Qt/Wayland)
add_executable(ringlight src/ringlight.cpp)
target_link_libraries(ringlight PRIVATE
    Qt6::Core
    Qt6::Gui
    LayerShellQt::Interface
)

# RingLight Monitor (pure C)
add_executable(ringlight-monitor src/ringlight-monitor.c)

# Installation
include(GNUInstallDirs)
install(TARGETS ringlight ringlight-monitor
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES systemd/ringlight-monitor.service
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user
)
