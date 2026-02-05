cmake_minimum_required(VERSION 3.16)
project(ringlight VERSION 1.0)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Backend options
option(ENABLE_WAYLAND "Build with Wayland backend support" ON)
option(ENABLE_X11 "Build with X11 backend support" ON)

if(NOT ENABLE_WAYLAND AND NOT ENABLE_X11)
    message(FATAL_ERROR "At least one backend (ENABLE_WAYLAND or ENABLE_X11) must be enabled")
endif()

# Find packages
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(PkgConfig REQUIRED)

# Overlay sources (always includes the main orchestrator)
set(OVERLAY_SOURCES src/overlay.c)
set(OVERLAY_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/src)
set(OVERLAY_LIBRARIES)
set(OVERLAY_DEFINES)

# Wayland backend
if(ENABLE_WAYLAND)
    pkg_check_modules(WAYLAND REQUIRED wayland-client)

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

    list(APPEND OVERLAY_SOURCES src/overlay_wayland.c ${XDG_SHELL_C} ${LAYER_SHELL_C})
    list(APPEND OVERLAY_INCLUDE_DIRS ${PROTO_BUILD_DIR} ${WAYLAND_INCLUDE_DIRS})
    list(APPEND OVERLAY_LIBRARIES ${WAYLAND_LIBRARIES} rt)
    list(APPEND OVERLAY_DEFINES HAVE_WAYLAND)
endif()

# X11 backend
if(ENABLE_X11)
    find_package(X11 REQUIRED COMPONENTS Xrandr)

    list(APPEND OVERLAY_SOURCES src/overlay_x11.c)
    list(APPEND OVERLAY_INCLUDE_DIRS ${X11_INCLUDE_DIR})
    list(APPEND OVERLAY_LIBRARIES ${X11_LIBRARIES} ${X11_Xrandr_LIB})
    list(APPEND OVERLAY_DEFINES HAVE_X11)
endif()

# Overlay executable
add_executable(ringlight-overlay ${OVERLAY_SOURCES})
target_include_directories(ringlight-overlay PRIVATE ${OVERLAY_INCLUDE_DIRS})
target_link_libraries(ringlight-overlay PRIVATE ${OVERLAY_LIBRARIES})
target_compile_definitions(ringlight-overlay PRIVATE ${OVERLAY_DEFINES})

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
