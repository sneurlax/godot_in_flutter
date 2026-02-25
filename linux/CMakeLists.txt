cmake_minimum_required(VERSION 3.10)
project(flutter_godot LANGUAGES C CXX)

# This value is used when generating builds using this plugin, so it must
# not be changed
set(PLUGIN_NAME "flutter_godot_plugin")

# =============================================================================
# Find required packages
# =============================================================================
find_package(PkgConfig REQUIRED)
pkg_check_modules(EPOXY REQUIRED epoxy)

# =============================================================================
# Main Flutter plugin (includes libgodot bridge and GL texture)
# =============================================================================

add_library(${PLUGIN_NAME} SHARED
  "flutter_godot.cc"
  "godot_gl_texture.cc"
  "libgodot_bridge.c"
)

apply_standard_settings(${PLUGIN_NAME})

set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  C_STANDARD 11
  CXX_STANDARD 17
)

target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)

# Suppress some warnings that are noisy but not errors
target_compile_options(${PLUGIN_NAME} PRIVATE -Wall -Wno-error)

target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_include_directories(${PLUGIN_NAME} PRIVATE
  ${EPOXY_INCLUDE_DIRS}
)

target_link_libraries(${PLUGIN_NAME} PRIVATE
  flutter
  PkgConfig::GTK
  ${EPOXY_LIBRARIES}
  dl
  pthread
)

# =============================================================================
# Bundle libgodot.so if available
# =============================================================================
set(LIBGODOT_PATH "$ENV{LIBGODOT_PATH}")
if(NOT LIBGODOT_PATH)
  # Try the default build location
  set(LIBGODOT_DEFAULT_PATH "/tmp/libgodot-migeran/godot/bin/libgodot.linuxbsd.template_release.x86_64.so")
  if(EXISTS "${LIBGODOT_DEFAULT_PATH}")
    set(LIBGODOT_PATH "${LIBGODOT_DEFAULT_PATH}")
  endif()
endif()

if(LIBGODOT_PATH AND EXISTS "${LIBGODOT_PATH}")
  message(STATUS "Bundling libgodot.so from: ${LIBGODOT_PATH}")
  set(flutter_godot_bundled_libraries
    "${LIBGODOT_PATH}"
    PARENT_SCOPE
  )
else()
  message(STATUS "libgodot.so not found - will need to be provided at runtime via LIBGODOT_PATH env or libgodotPath parameter")
  set(flutter_godot_bundled_libraries
    ""
    PARENT_SCOPE
  )
endif()
