cmake_minimum_required(VERSION 3.5.0)
project(OhosWayland)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(OHOS_WAYLAND_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(WAYLAND_LIBS_DIR ${OHOS_WAYLAND_ROOT}/../../../libs/${OHOS_ARCH})
set(GPU_PREFIX ${OHOS_WAYLAND_ROOT}/../../../../native-build/native_${OHOS_ARCH}/gpu-install)

include_directories(
    ${OHOS_WAYLAND_ROOT}
    ${OHOS_WAYLAND_ROOT}/protocols
    ${OHOS_WAYLAND_ROOT}/compositor/frame
    ${OHOS_WAYLAND_ROOT}/compositor/toplevel
    ${OHOS_WAYLAND_ROOT}/compositor/input
)

add_library(entry SHARED
    bridge/napi_init.cpp
    bridge/plugin_manager.cpp
    common/fs_utils.cpp
    common/perf_utils.cpp
    compositor/wayland_server.cpp
    compositor/wl_core.cpp
    compositor/xdg_shell.cpp
    compositor/data_device.cpp
    compositor/frame/compositor_blit.cpp
    compositor/frame/compositor_utils.cpp
    compositor/frame/frame_composer.cpp
    compositor/frame/frame_pipeline.cpp
    compositor/frame/geometry.cpp
    compositor/frame/shm_frame_source.cpp
    compositor/frame/zc_bridge.cpp
    compositor/input/input_injector.cpp
    compositor/input/input_queue.cpp
    compositor/input/input_resolver.cpp
    compositor/input/input_space_mapper.cpp
    compositor/input/input_state_tracker.cpp
    compositor/toplevel/desktop_compositor.cpp
    compositor/toplevel/move_grab.cpp
    compositor/toplevel/popup_manager.cpp
    compositor/toplevel/toplevel_event_bus.cpp
    compositor/toplevel/toplevel_manager.cpp
    graphics/egl_renderer.cpp
    graphics/gpu_frame_consumer.cpp
    graphics/direct_scene.cpp
    graphics/graphics_broker.cpp
    graphics/present_stats.cpp
    graphics/shader_utils.cpp
    input/coasting.cpp
    input/input_manager.cpp
    input/pointer_extras.cpp
    input/seat.cpp
    input/text_input.cpp
    runtime/app_registry.cpp
    runtime/native_child_launcher.cpp
    runtime/tar_extract.cpp
    compositor/xdg_decoration.cpp
    protocols/xdg-decoration-unstable-v1-protocol.c
    compositor/titlebar.cpp
    compositor/portal/session_bus.cpp
    protocols/ohos-titlebar-v1-protocol.c
    protocols/xdg-shell-protocol.c
    protocols/viewporter-server-protocol.c
    protocols/pointer-warp-v1-protocol.c
    protocols/pointer-constraints-unstable-v1-protocol.c
    protocols/relative-pointer-unstable-v1-protocol.c
    protocols/text-input-unstable-v3-protocol.c
)

set_source_files_properties(compositor/frame/compositor_blit.cpp PROPERTIES COMPILE_OPTIONS "-O2")

target_link_libraries(entry PUBLIC
    libace_napi.z.so
    libace_ndk.z.so
    libnative_window.so
    libnative_image.so
    libnative_vsync.so
    libnative_window_manager.so
    libEGL.so
    libGLESv3.so
    libhilog_ndk.z.so
    libchild_process.so
    libipc_capi.so
    libpasteboard.so
    libudmf.so
    libability_access_control.so
    -L/usr/lib
    -l:libcrypto.so.3
    ${WAYLAND_LIBS_DIR}/libwayland-server.so.0
)

add_library(native_child SHARED
    runtime/native_demo.cpp
    runtime/launch_params.cpp
    protocols/xdg-shell-protocol.c
)
set_target_properties(native_child PROPERTIES OUTPUT_NAME native_demo)
target_link_libraries(native_child PUBLIC
    libhilog_ndk.z.so
    ${WAYLAND_LIBS_DIR}/libwayland-client.so.0
)

add_library(native_gl_child SHARED
    runtime/native_gl_demo.cpp
    runtime/launch_params.cpp
    protocols/xdg-shell-protocol.c
)
set_target_properties(native_gl_child PROPERTIES OUTPUT_NAME native_gl_demo)
target_include_directories(native_gl_child PRIVATE ${GPU_PREFIX}/include)
target_link_libraries(native_gl_child PUBLIC
    libhilog_ndk.z.so
    -L${GPU_PREFIX}/lib
    -l:libEGL.so.1
    -lwayland-egl
    ${WAYLAND_LIBS_DIR}/libwayland-client.so.0
)

set(GTK_PREFIX ${OHOS_WAYLAND_ROOT}/../../../../native-build/gtk-install)
set(ENV{PKG_CONFIG_PATH} "$ENV{HOME}/.harmonybrew/lib/pkgconfig:$ENV{HOME}/.harmonybrew/share/pkgconfig:${GPU_PREFIX}/lib/pkgconfig:${GPU_PREFIX}/share/pkgconfig:${GTK_PREFIX}/lib/pkgconfig")
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
# The apps' D-Bus session bus and xdg-desktop-portal (compositor/portal).
# Harmonybrew's GLib, the one the app bottles ship (ohos_wl.py library_dirs),
# so the HAP holds a single GLib.
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
target_link_directories(entry PRIVATE ${GIO_LIBRARY_DIRS} $ENV{HOME}/.harmonybrew/lib)
target_link_libraries(entry PUBLIC PkgConfig::GIO)
add_library(native_gtk_child SHARED
    runtime/native_gtk_demo.cpp
    runtime/launch_params.cpp
)
set_target_properties(native_gtk_child PROPERTIES OUTPUT_NAME native_gtk_demo)
target_link_directories(native_gtk_child PRIVATE ${GTK4_LIBRARY_DIRS})
target_link_libraries(native_gtk_child PUBLIC
    libhilog_ndk.z.so
    ${WAYLAND_LIBS_DIR}/libwayland-client.so.0
    PkgConfig::GTK4
)

add_library(ohos_direct_child SHARED runtime/direct_child.cpp runtime/elf_entry.cpp runtime/launch_params.cpp
    runtime/remote_wsi_host.cpp)
target_link_libraries(ohos_direct_child PUBLIC
    libhilog_ndk.z.so
    libnative_window.so
    libchild_process.so
    libipc_capi.so
)

add_library(ohos_libgl SHARED runtime/libgl_alias.c)
set_target_properties(ohos_libgl PROPERTIES OUTPUT_NAME GL SUFFIX ".so.1" NO_SONAME FALSE)
target_link_options(ohos_libgl PRIVATE -Wl,-soname,libGL.so.1 -Wl,--no-as-needed)
target_link_libraries(ohos_libgl PRIVATE ${WAYLAND_LIBS_DIR}/libGLESv2.so.2)

# Loaded into programs the direct child execs from the app's HNP: the
# sandbox workarounds (preloaded) and the Vulkan library Zink uses there.
add_library(ohos_exec SHARED runtime/exec_preload.cpp)
# Preloaded into stock programs: keep the C++ runtime out of them.
target_compile_options(ohos_exec PRIVATE -fno-exceptions -fno-rtti -fno-threadsafe-statics)
target_link_options(ohos_exec PRIVATE -nostdlib++)
add_library(ohos_vkwsi SHARED runtime/remote_wsi_client.cpp)
target_link_libraries(ohos_vkwsi PUBLIC libhilog_ndk.z.so)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/runtime/ssh_client.cpp)
    add_library(ghostty_ssh SHARED runtime/ssh_client.cpp)
    target_include_directories(ghostty_ssh PRIVATE $ENV{HOME}/.harmonybrew/include)
    target_link_libraries(ghostty_ssh PUBLIC
        -L$ENV{HOME}/.harmonybrew/lib
        -l:libssh2.so.1
    )
    set_target_properties(ghostty_ssh PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN")
endif()

set_target_properties(entry native_child native_gl_child native_gtk_child ohos_direct_child ohos_exec ohos_vkwsi PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN"
)

# Native HarmonyOS title bars for GTK apps (bottled with any app that links GTK 4).
add_library(ohos_titlebar SHARED
    runtime/gtk_titlebar_bridge.cpp
    protocols/ohos-titlebar-v1-protocol.c
)
target_link_directories(ohos_titlebar PRIVATE ${GTK4_LIBRARY_DIRS})
target_link_libraries(ohos_titlebar PUBLIC
    ${WAYLAND_LIBS_DIR}/libwayland-client.so.0
    PkgConfig::GTK4
)
set_target_properties(ohos_titlebar PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN")
