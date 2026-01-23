cmake_minimum_required(VERSION 3.16)
project(ringlight VERSION 1.0)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Find packages
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED wayland-client)

# Find wayland-scanner
find_program(WAYLAND_SCANNER wayland-scanner)
if(NOT WAYLAND_SCANNER)
    message(FATAL_ERROR "wayland-scanner not found")
endif()

# Protocol paths
pkg_get_variable(WL_PROTOCOL_DIR wayland-protocols pkgdatadir)
set(WLR_PROTOCOLS_DIR "${CMAKE_SOURCE_DIR}/protocols")

# Generate protocol sources
set(PROTO_BUILD_DIR "${CMAKE_BINARY_DIR}/protocols")
file(MAKE_DIRECTORY ${PROTO_BUILD_DIR})

# xdg-shell protocol
set(XDG_SHELL_XML "${WL_PROTOCOL_DIR}/stable/xdg-shell/xdg-shell.xml")
set(XDG_SHELL_C "${PROTO_BUILD_DIR}/xdg-shell-client.c")
set(XDG_SHELL_H "${PROTO_BUILD_DIR}/xdg-shell-client.h")
add_custom_command(
    OUTPUT ${XDG_SHELL_C} ${XDG_SHELL_H}
    COMMAND ${WAYLAND_SCANNER} client-header ${XDG_SHELL_XML} ${XDG_SHELL_H}
    COMMAND ${WAYLAND_SCANNER} private-code ${XDG_SHELL_XML} ${XDG_SHELL_C}
    DEPENDS ${XDG_SHELL_XML}
)

# wlr-layer-shell protocol (bundled)
set(LAYER_SHELL_XML "${WLR_PROTOCOLS_DIR}/wlr-layer-shell-unstable-v1.xml")
set(LAYER_SHELL_C "${PROTO_BUILD_DIR}/wlr-layer-shell-unstable-v1-client.c")
set(LAYER_SHELL_H "${PROTO_BUILD_DIR}/wlr-layer-shell-unstable-v1-client.h")
add_custom_command(
    OUTPUT ${LAYER_SHELL_C} ${LAYER_SHELL_H}
    COMMAND ${WAYLAND_SCANNER} client-header ${LAYER_SHELL_XML} ${LAYER_SHELL_H}
    COMMAND ${WAYLAND_SCANNER} private-code ${LAYER_SHELL_XML} ${LAYER_SHELL_C}
    DEPENDS ${LAYER_SHELL_XML}
)

# Overlay (C with Wayland)
add_executable(ringlight-overlay
    src/overlay.c
    ${XDG_SHELL_C}
    ${LAYER_SHELL_C}
)
target_include_directories(ringlight-overlay PRIVATE ${PROTO_BUILD_DIR} ${WAYLAND_INCLUDE_DIRS})
target_link_libraries(ringlight-overlay PRIVATE ${WAYLAND_LIBRARIES} rt)

# GUI (C++ with Qt)
add_executable(ringlight-gui src/gui.cpp)
target_link_libraries(ringlight-gui PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)

# Monitor daemon (pure C)
add_executable(ringlight-monitor src/monitor.c)
target_link_libraries(ringlight-monitor PRIVATE)

# Install binaries
install(TARGETS ringlight-overlay ringlight-gui ringlight-monitor RUNTIME DESTINATION bin)

# Install wrapper script
install(PROGRAMS ringlight-monitor-wrapper DESTINATION bin)

# Install systemd user service
install(FILES ringlight-monitor.service DESTINATION lib/systemd/user)

# Install desktop file
install(FILES resources/ringlight.desktop DESTINATION share/applications)
