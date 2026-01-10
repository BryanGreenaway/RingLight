cmake_minimum_required(VERSION 3.16)
project(ringlight VERSION 1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Find required packages
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)

# Find wayland-scanner
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

# Find wayland-protocols directory
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
if(NOT WAYLAND_PROTOCOLS_DIR)
    # Fallback paths
    if(EXISTS "/usr/share/wayland-protocols")
        set(WAYLAND_PROTOCOLS_DIR "/usr/share/wayland-protocols")
    else()
        message(FATAL_ERROR "wayland-protocols not found")
    endif()
endif()

# Generate layer-shell protocol code
set(PROTOCOL_DIR ${CMAKE_SOURCE_DIR}/protocol)
set(GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_DIR})

# XDG Shell (required by layer-shell)
set(XDG_SHELL_XML ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml)
set(XDG_SHELL_CLIENT_H ${GENERATED_DIR}/xdg-shell-client.h)
set(XDG_SHELL_CODE_C ${GENERATED_DIR}/xdg-shell-code.c)

add_custom_command(
    OUTPUT ${XDG_SHELL_CLIENT_H}
    COMMAND ${WAYLAND_SCANNER} client-header ${XDG_SHELL_XML} ${XDG_SHELL_CLIENT_H}
    DEPENDS ${XDG_SHELL_XML}
    COMMENT "Generating xdg-shell client header"
)

add_custom_command(
    OUTPUT ${XDG_SHELL_CODE_C}
    COMMAND ${WAYLAND_SCANNER} private-code ${XDG_SHELL_XML} ${XDG_SHELL_CODE_C}
    DEPENDS ${XDG_SHELL_XML}
    COMMENT "Generating xdg-shell protocol code"
)

# Layer shell
set(LAYER_SHELL_XML ${PROTOCOL_DIR}/wlr-layer-shell-unstable-v1.xml)
set(LAYER_SHELL_CLIENT_H ${GENERATED_DIR}/wlr-layer-shell-unstable-v1-client.h)
set(LAYER_SHELL_CODE_C ${GENERATED_DIR}/wlr-layer-shell-unstable-v1-code.c)

add_custom_command(
    OUTPUT ${LAYER_SHELL_CLIENT_H}
    COMMAND ${WAYLAND_SCANNER} client-header ${LAYER_SHELL_XML} ${LAYER_SHELL_CLIENT_H}
    DEPENDS ${LAYER_SHELL_XML}
    COMMENT "Generating layer-shell client header"
)

add_custom_command(
    OUTPUT ${LAYER_SHELL_CODE_C}
    COMMAND ${WAYLAND_SCANNER} private-code ${LAYER_SHELL_XML} ${LAYER_SHELL_CODE_C}
    DEPENDS ${LAYER_SHELL_XML}
    COMMENT "Generating layer-shell protocol code"
)

add_custom_target(wayland_protocols DEPENDS 
    ${XDG_SHELL_CLIENT_H} ${XDG_SHELL_CODE_C}
    ${LAYER_SHELL_CLIENT_H} ${LAYER_SHELL_CODE_C}
)

# === Overlay (pure C, no Qt window management) ===
add_executable(ringlight-overlay 
    src/overlay.c 
    ${XDG_SHELL_CODE_C}
    ${LAYER_SHELL_CODE_C}
)
add_dependencies(ringlight-overlay wayland_protocols)
target_include_directories(ringlight-overlay PRIVATE 
    ${GENERATED_DIR}
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
)
target_link_libraries(ringlight-overlay PRIVATE
    ${WAYLAND_CLIENT_LIBRARIES}
    rt  # for shm_open
)
target_compile_options(ringlight-overlay PRIVATE ${WAYLAND_CLIENT_CFLAGS_OTHER})

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
