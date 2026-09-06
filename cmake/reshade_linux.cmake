include(GNUInstallDirs)

set(RESHADE_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${RESHADE_GENERATED_INCLUDE_DIR}")

set(RESHADE_VERSION "" CACHE STRING "ReShade version used when Git tag metadata is unavailable")
if(RESHADE_VERSION)
  set(RESHADE_VERSION_TAG "v${RESHADE_VERSION}")
else()
  execute_process(
    COMMAND git describe --tags --match "v[0-9]*" --abbrev=0
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE RESHADE_VERSION_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif()
if(NOT RESHADE_VERSION_TAG MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
  message(FATAL_ERROR "Unable to determine the ReShade version; configure with -DRESHADE_VERSION=<major.minor.patch>")
endif()
set(RESHADE_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(RESHADE_VERSION_MINOR "${CMAKE_MATCH_2}")
set(RESHADE_VERSION_REVISION "${CMAKE_MATCH_3}")
file(WRITE "${RESHADE_GENERATED_INCLUDE_DIR}/version.h"
"#pragma once

#define VERSION_FULL ${RESHADE_VERSION_MAJOR}.${RESHADE_VERSION_MINOR}.${RESHADE_VERSION_REVISION}.0
#define VERSION_MAJOR ${RESHADE_VERSION_MAJOR}
#define VERSION_MINOR ${RESHADE_VERSION_MINOR}
#define VERSION_REVISION ${RESHADE_VERSION_REVISION}
#define VERSION_BUILD 0

#define VERSION_STRING_FILE \"${RESHADE_VERSION_MAJOR}.${RESHADE_VERSION_MINOR}.${RESHADE_VERSION_REVISION}.0\"
#define VERSION_STRING_PRODUCT \"${RESHADE_VERSION_MAJOR}.${RESHADE_VERSION_MINOR}.${RESHADE_VERSION_REVISION}\"
")

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_package(Threads REQUIRED)

# Generate xdg-output bindings used to reconcile logical Wayland pointer coordinates
# with physical Vulkan swapchain dimensions under fractional display scaling.
set(RESHADE_WAYLAND_XDG_OUTPUT_XML "${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-output/xdg-output-unstable-v1.xml")
if(NOT EXISTS "${RESHADE_WAYLAND_XDG_OUTPUT_XML}")
  message(FATAL_ERROR "Wayland xdg-output protocol XML not found: ${RESHADE_WAYLAND_XDG_OUTPUT_XML}")
endif()
set(RESHADE_WAYLAND_XDG_OUTPUT_HEADER "${RESHADE_GENERATED_INCLUDE_DIR}/xdg-output-unstable-v1-client-protocol.h")
set(RESHADE_WAYLAND_XDG_OUTPUT_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/xdg-output-unstable-v1-protocol.c")
add_custom_command(
  OUTPUT "${RESHADE_WAYLAND_XDG_OUTPUT_HEADER}"
  COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${RESHADE_WAYLAND_XDG_OUTPUT_XML}" "${RESHADE_WAYLAND_XDG_OUTPUT_HEADER}"
  DEPENDS "${RESHADE_WAYLAND_XDG_OUTPUT_XML}"
)
add_custom_command(
  OUTPUT "${RESHADE_WAYLAND_XDG_OUTPUT_SOURCE}"
  COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${RESHADE_WAYLAND_XDG_OUTPUT_XML}" "${RESHADE_WAYLAND_XDG_OUTPUT_SOURCE}"
  DEPENDS "${RESHADE_WAYLAND_XDG_OUTPUT_XML}"
)
set(RESHADE_WAYLAND_RELATIVE_POINTER_XML "${WAYLAND_PROTOCOLS_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml")
set(RESHADE_WAYLAND_RELATIVE_POINTER_HEADER "${RESHADE_GENERATED_INCLUDE_DIR}/relative-pointer-unstable-v1-client-protocol.h")
set(RESHADE_WAYLAND_RELATIVE_POINTER_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/relative-pointer-unstable-v1-protocol.c")
add_custom_command(
  OUTPUT "${RESHADE_WAYLAND_RELATIVE_POINTER_HEADER}"
  COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${RESHADE_WAYLAND_RELATIVE_POINTER_XML}" "${RESHADE_WAYLAND_RELATIVE_POINTER_HEADER}"
  DEPENDS "${RESHADE_WAYLAND_RELATIVE_POINTER_XML}"
)
add_custom_command(
  OUTPUT "${RESHADE_WAYLAND_RELATIVE_POINTER_SOURCE}"
  COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${RESHADE_WAYLAND_RELATIVE_POINTER_XML}" "${RESHADE_WAYLAND_RELATIVE_POINTER_SOURCE}"
  DEPENDS "${RESHADE_WAYLAND_RELATIVE_POINTER_XML}"
)
set_source_files_properties(source/linux/input_linux.cpp PROPERTIES OBJECT_DEPENDS "${RESHADE_WAYLAND_XDG_OUTPUT_HEADER};${RESHADE_WAYLAND_RELATIVE_POINTER_HEADER}")

# Generate a native lookup table from the same localization resources used by Windows.
file(GLOB RESHADE_LOCALIZATION_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/lang_*.rc2")
if(NOT RESHADE_LOCALIZATION_SOURCES)
  message(FATAL_ERROR "No ReShade localization sources found in ${CMAKE_CURRENT_SOURCE_DIR}/res")
endif()
set(RESHADE_LOCALIZATION_HEADER "${RESHADE_GENERATED_INCLUDE_DIR}/localization_linux.hpp")
set(RESHADE_LOCALIZATION_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/localization_linux.cpp")
add_custom_command(
  OUTPUT "${RESHADE_LOCALIZATION_HEADER}" "${RESHADE_LOCALIZATION_SOURCE}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/source/linux/generate_localization.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/res" "${RESHADE_LOCALIZATION_HEADER}" "${RESHADE_LOCALIZATION_SOURCE}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/linux/generate_localization.py" ${RESHADE_LOCALIZATION_SOURCES}
)
set_source_files_properties(source/linux/runtime_platform.cpp PROPERTIES OBJECT_DEPENDS "${RESHADE_LOCALIZATION_HEADER}")

# Embed the existing ReShade ImGui shaders without relying on Win32 resources.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/res/shaders/imgui_vs_450.spv" RESHADE_IMGUI_VS_SPIRV HEX)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/res/shaders/imgui_ps_450.spv" RESHADE_IMGUI_PS_SPIRV HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," RESHADE_IMGUI_VS_SPIRV "${RESHADE_IMGUI_VS_SPIRV}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," RESHADE_IMGUI_PS_SPIRV "${RESHADE_IMGUI_PS_SPIRV}")
configure_file(source/linux/resources_linux.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/resources_linux.hpp @ONLY)

function(reshade_configure_linux_target target)
  set_target_properties(${target} PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${RESHADE_GENERATED_INCLUDE_DIR}")
  target_sources(
    ${target}
    PRIVATE
      source/ini_file.cpp
      source/addon.cpp
      source/addon.hpp
      source/addon_manager.cpp
      source/addon_manager.hpp
      source/input.cpp
      source/runtime.cpp
      source/runtime_api.cpp
      source/runtime_manager.cpp
      source/state_block.cpp
      source/imgui_function_table.cpp
      source/imgui_function_table_18600.cpp
      source/imgui_function_table_18971.cpp
      source/imgui_function_table_19000.cpp
      source/imgui_function_table_19040.cpp
      source/imgui_function_table_19180.cpp
      source/imgui_function_table_19191.cpp
      source/imgui_function_table_19222.cpp
      source/imgui_function_table_19250.cpp
      ${RESHADE_SOURCE_VULKAN}
      ${RESHADE_WAYLAND_XDG_OUTPUT_HEADER}
      ${RESHADE_WAYLAND_XDG_OUTPUT_SOURCE}
      ${RESHADE_WAYLAND_RELATIVE_POINTER_HEADER}
      ${RESHADE_WAYLAND_RELATIVE_POINTER_SOURCE}
      ${RESHADE_LOCALIZATION_HEADER}
      ${RESHADE_LOCALIZATION_SOURCE}
      source/imgui_code_editor.cpp
      source/imgui_widgets.cpp
      source/dll_log.cpp
      source/linux/input_linux.cpp
      source/linux/platform_utils.cpp
      source/linux/process_environment.cpp
      source/linux/runtime_platform.cpp
      source/runtime_gui.cpp
  )
  target_compile_definitions(
    ${target}
    PRIVATE
      RESHADE_GUI=1
      RESHADE_API_LIBRARY_EXPORT
      RESHADE_ADDON=2
      RESHADE_LOCALIZATION
      $<$<CONFIG:Debug>:RESHADE_VERBOSE_LOG>
      $<$<CONFIG:Debug>:_DEBUG>
      $<$<CONFIG:Release>:NDEBUG>
  )
  set_source_files_properties(examples/09-depth/generic_depth_addon.cpp PROPERTIES COMPILE_DEFINITIONS BUILTIN_ADDON)
  set_source_files_properties(examples/09-depth/generic_depth_addon.cpp PROPERTIES COMPILE_FLAGS "-include reshade.hpp")
  target_sources(${target} PRIVATE examples/09-depth/generic_depth_addon.cpp)
  target_link_libraries(
    ${target}
    PRIVATE
      ReShadeFX fpng glad ImGui jxl stb utfcpp VMA Threads::Threads ${CMAKE_DL_LIBS}
      PkgConfig::WAYLAND_CLIENT PkgConfig::XKBCOMMON PkgConfig::FONTCONFIG
  )
  target_link_options(${target} PRIVATE -Wl,--no-undefined)

  set(RESHADE_LAYER_LIBRARY_PATH "../../../${CMAKE_INSTALL_LIBDIR}/reshade/ReShade${RESHADE_SUFFIX}${CMAKE_SHARED_LIBRARY_SUFFIX}")
  configure_file(res/reshade_layer.json.in ${CMAKE_CURRENT_BINARY_DIR}/ReShade64.json @ONLY)
  install(TARGETS ${target} LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/reshade")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ReShade64.json" DESTINATION "${CMAKE_INSTALL_DATADIR}/vulkan/implicit_layer.d")
endfunction()

option(RESHADE_BUILD_LINUX_ADDON_EXAMPLES "Build native Linux add-on examples" OFF)

if(RESHADE_BUILD_LINUX_ADDON_EXAMPLES)
  # Native add-ons use the same API-library ABI as Windows add-ons. The small
  # compatibility header only supplies the Win32 spellings still present in
  # the example sources (DllMain, module paths and secure CRT formatting).
  set(RESHADE_LINUX_ADDON_COMPAT_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/source/linux/addon_compat.hpp")
  set(RESHADE_LINUX_ADDON_ENTRYPOINT "${CMAKE_CURRENT_SOURCE_DIR}/source/linux/addon_entrypoint.cpp")

  function(reshade_configure_linux_addon target output_name source_file)
    cmake_parse_arguments(ARG "NO_ENTRYPOINT" "" "SOURCES;LIBRARIES" ${ARGN})

    add_library(${target} MODULE ${source_file} ${ARG_SOURCES})
    set_target_properties(
      ${target}
      PROPERTIES
        PREFIX ""
        OUTPUT_NAME "${output_name}"
        SUFFIX ".addon${RESHADE_SUFFIX}"
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
        BUILD_RPATH "$<TARGET_FILE_DIR:ReShade>"
        INSTALL_RPATH "$ORIGIN/../../${CMAKE_INSTALL_LIBDIR}/reshade"
    )

    target_include_directories(
      ${target}
      PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/source"
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/utils"
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/imgui"
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/stb"
    )
    target_compile_definitions(
      ${target}
      PRIVATE
        RESHADE_API_LIBRARY=1
        ImTextureID=ImU64
    )
    target_compile_options(${target} PRIVATE "-include${RESHADE_LINUX_ADDON_COMPAT_HEADER}" -Wno-changes-meaning -UBUILTIN_ADDON)
    if(NOT ARG_NO_ENTRYPOINT)
      target_sources(${target} PRIVATE ${RESHADE_LINUX_ADDON_ENTRYPOINT})
      target_compile_definitions(${target} PRIVATE DllMain=ReShadeLinuxAddonDllMain)
    endif()
    target_link_libraries(${target} PRIVATE ReShade Threads::Threads ${ARG_LIBRARIES})
    target_link_options(${target} PRIVATE -Wl,--no-undefined)

    install(TARGETS ${target} LIBRARY DESTINATION "${CMAKE_INSTALL_DATADIR}/reshade")
  endfunction()

  reshade_configure_linux_addon(reshade_addon_fps_limit fps_limit examples/01-fps_limit/fps_limit_addon.cpp)
  reshade_configure_linux_addon(reshade_addon_history_window history_window examples/03-history_window/history_window_addon.cpp)
  reshade_configure_linux_addon(reshade_addon_api_trace api_trace examples/04-api_trace/api_trace_addon.cpp)
  reshade_configure_linux_addon(reshade_addon_shader_dump shader_dump examples/05-shader_dump/shader_dump_addon.cpp)
  reshade_configure_linux_addon(reshade_addon_shader_replace shader_replace examples/06-shader_replace/shader_replace_addon.cpp)
  reshade_configure_linux_addon(
    reshade_addon_texture_dump texture_dump examples/07-texture_dump/texture_dump_addon.cpp
    SOURCES examples/utils/save_texture_image.cpp
  )
  reshade_configure_linux_addon(
    reshade_addon_texture_replace texture_replace examples/08-texture_replace/texture_replace_addon.cpp
    SOURCES examples/utils/load_texture_image.cpp
  )
  reshade_configure_linux_addon(reshade_addon_generic_depth generic_depth examples/09-depth/generic_depth_addon.cpp)
  reshade_configure_linux_addon(
    reshade_addon_texture_overlay texture_overlay examples/10-texture_overlay/texture_overlay_addon.cpp
    SOURCES examples/utils/descriptor_tracking.cpp examples/utils/save_texture_image.cpp
  )
  reshade_configure_linux_addon(
    reshade_addon_effects_during_frame effects_during_frame examples/13-effects_during_frame/effects_during_frame_addon.cpp
    SOURCES examples/utils/state_tracking.cpp
  )
  reshade_configure_linux_addon(reshade_addon_runtime_sync runtime_sync examples/15-effect_runtime_sync/runtime_sync_addon.cpp)
  reshade_configure_linux_addon(reshade_addon_swapchain_override swapchain_override examples/16-swapchain_override/swapchain_override_addon.cpp)

  # Video capture has an optional FFmpeg dependency and already uses the
  # AddonInit/AddOnUninit entry points, so do not add the DllMain shim.
  pkg_check_modules(FFMPEG IMPORTED_TARGET libavcodec libavformat libavutil)
  if(FFMPEG_FOUND)
    reshade_configure_linux_addon(
      reshade_addon_video_capture video_capture examples/12-video_capture/video_capture.cpp
      NO_ENTRYPOINT
      LIBRARIES PkgConfig::FFMPEG
    )
  else()
    message(STATUS "Skipping Linux video capture add-on: FFmpeg development files were not found")
  endif()

  find_program(RESHADE_DXC_EXECUTABLE NAMES dxc)
  if(RESHADE_DXC_EXECUTABLE)
    set(RESHADE_RAY_TRACING_SHADER "${CMAKE_CURRENT_BINARY_DIR}/ray_tracing_shaders.spv")
    add_custom_command(
      OUTPUT "${RESHADE_RAY_TRACING_SHADER}"
      COMMAND "${RESHADE_DXC_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/examples/14-ray_tracing/ray_tracing_shaders.hlsl"
        -T lib_6_5 -Fo "${RESHADE_RAY_TRACING_SHADER}" -spirv -fspv-target-env=vulkan1.1spirv1.4
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/14-ray_tracing/ray_tracing_shaders.hlsl"
      VERBATIM
    )
    add_custom_target(reshade_ray_tracing_shaders DEPENDS "${RESHADE_RAY_TRACING_SHADER}")
    reshade_configure_linux_addon(reshade_addon_ray_tracing ray_tracing examples/14-ray_tracing/ray_tracing_addon.cpp)
    add_dependencies(reshade_addon_ray_tracing reshade_ray_tracing_shaders)
    install(FILES "${RESHADE_RAY_TRACING_SHADER}" DESTINATION "${CMAKE_INSTALL_DATADIR}/reshade")
  else()
    message(STATUS "Skipping Linux ray tracing add-on: DXC was not found (required for lib_6_5 shader compilation)")
  endif()
endif()
