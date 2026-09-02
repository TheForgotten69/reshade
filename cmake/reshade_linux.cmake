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
set_source_files_properties(source/linux/input_linux.cpp PROPERTIES OBJECT_DEPENDS "${RESHADE_WAYLAND_XDG_OUTPUT_HEADER}")

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
      source/input.cpp
      source/runtime.cpp
      source/runtime_api.cpp
      source/runtime_manager.cpp
      source/state_block.cpp
      ${RESHADE_SOURCE_VULKAN}
      ${RESHADE_WAYLAND_XDG_OUTPUT_HEADER}
      ${RESHADE_WAYLAND_XDG_OUTPUT_SOURCE}
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
      RESHADE_ADDON=0
      RESHADE_LOCALIZATION
      $<$<CONFIG:Debug>:RESHADE_VERBOSE_LOG>
      $<$<CONFIG:Debug>:_DEBUG>
      $<$<CONFIG:Release>:NDEBUG>
  )
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
