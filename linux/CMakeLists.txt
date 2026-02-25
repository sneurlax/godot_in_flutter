cmake_minimum_required(VERSION 3.10)
project(flutter_godot LANGUAGES C CXX)

# This value is used when generating builds using this plugin, so it must
# not be changed
set(PLUGIN_NAME "flutter_godot_plugin")

add_library(${PLUGIN_NAME} SHARED
  "flutter_godot.cc"
)

apply_standard_settings(${PLUGIN_NAME})

set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)

target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(${PLUGIN_NAME} PRIVATE flutter)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::GTK)

# WebKit2GTK for lightweight webview (uses system webkit, ~0 extra bundle size)
# Try pkg-config first, fall back to manual paths
find_package(PkgConfig QUIET)
pkg_check_modules(WEBKIT2GTK webkit2gtk-4.1)

if(WEBKIT2GTK_FOUND)
  target_include_directories(${PLUGIN_NAME} PRIVATE ${WEBKIT2GTK_INCLUDE_DIRS})
  target_link_libraries(${PLUGIN_NAME} PRIVATE ${WEBKIT2GTK_LIBRARIES})
  target_compile_definitions(${PLUGIN_NAME} PRIVATE HAS_WEBKIT2GTK=1)
  message(STATUS "flutter_godot: Using webkit2gtk-4.1 via pkg-config")
else()
  # Fall back: try to find headers in common locations
  # This handles the case where dev headers are extracted but pkg-config
  # isn't fully configured (e.g., missing transitive deps in .pc files)
  set(WEBKIT2GTK_MANUAL_INCLUDE_DIRS "")
  set(WEBKIT2GTK_MANUAL_FOUND FALSE)

  # Check /tmp/webkit2gtk-dev (extracted headers) or system paths
  # The include structure is: webkitgtk-4.1/webkit2/webkit2.h
  # So the include dir should be webkitgtk-4.1/ for #include <webkit2/webkit2.h>
  foreach(SEARCH_PATH "/tmp/webkit2gtk-dev/usr/include/webkitgtk-4.1" "/usr/include/webkitgtk-4.1")
    if(EXISTS "${SEARCH_PATH}/webkit2/webkit2.h")
      list(APPEND WEBKIT2GTK_MANUAL_INCLUDE_DIRS "${SEARCH_PATH}")
      set(WEBKIT2GTK_MANUAL_FOUND TRUE)
      break()
    endif()
  endforeach()

  if(WEBKIT2GTK_MANUAL_FOUND)
    target_include_directories(${PLUGIN_NAME} PRIVATE ${WEBKIT2GTK_MANUAL_INCLUDE_DIRS})

    # Also add transitive dependency include dirs if available locally
    foreach(DEP_INCLUDE
      "/tmp/webkit2gtk-dev/usr/include/libsoup-3.0"
      "/usr/include/libsoup-3.0"
      "/tmp/webkit2gtk-dev/usr/include/webkitgtk-4.1"
      "/usr/include/webkitgtk-4.1")
      if(EXISTS "${DEP_INCLUDE}")
        target_include_directories(${PLUGIN_NAME} PRIVATE "${DEP_INCLUDE}")
      endif()
    endforeach()

    # Try to find the actual .so files (the -dev package creates .so symlinks,
    # but if only runtime libs are installed, we have .so.0 files)
    find_library(WEBKIT2GTK_LIB NAMES webkit2gtk-4.1
      PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/lib64
      PATH_SUFFIXES "" NO_DEFAULT_PATH)
    find_library(JSCOREGTK_LIB NAMES javascriptcoregtk-4.1
      PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/lib64
      PATH_SUFFIXES "" NO_DEFAULT_PATH)

    # Fall back to .so.0 if .so not found
    if(NOT WEBKIT2GTK_LIB)
      find_library(WEBKIT2GTK_LIB NAMES libwebkit2gtk-4.1.so.0
        PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/lib64
        PATH_SUFFIXES "" NO_DEFAULT_PATH)
    endif()
    if(NOT JSCOREGTK_LIB)
      find_library(JSCOREGTK_LIB NAMES libjavascriptcoregtk-4.1.so.0
        PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/lib64
        PATH_SUFFIXES "" NO_DEFAULT_PATH)
    endif()

    # Last resort: use full paths directly
    if(NOT WEBKIT2GTK_LIB AND EXISTS "/usr/lib/x86_64-linux-gnu/libwebkit2gtk-4.1.so.0")
      set(WEBKIT2GTK_LIB "/usr/lib/x86_64-linux-gnu/libwebkit2gtk-4.1.so.0")
    endif()
    if(NOT JSCOREGTK_LIB AND EXISTS "/usr/lib/x86_64-linux-gnu/libjavascriptcoregtk-4.1.so.0")
      set(JSCOREGTK_LIB "/usr/lib/x86_64-linux-gnu/libjavascriptcoregtk-4.1.so.0")
    endif()

    if(WEBKIT2GTK_LIB AND JSCOREGTK_LIB)
      target_link_libraries(${PLUGIN_NAME} PRIVATE ${WEBKIT2GTK_LIB} ${JSCOREGTK_LIB})
      message(STATUS "flutter_godot: Linking webkit2gtk: ${WEBKIT2GTK_LIB}")
      message(STATUS "flutter_godot: Linking javascriptcoregtk: ${JSCOREGTK_LIB}")
    else()
      target_link_libraries(${PLUGIN_NAME} PRIVATE webkit2gtk-4.1 javascriptcoregtk-4.1)
    endif()
    target_compile_definitions(${PLUGIN_NAME} PRIVATE HAS_WEBKIT2GTK=1)
    message(STATUS "flutter_godot: Using webkit2gtk-4.1 via manual paths: ${WEBKIT2GTK_MANUAL_INCLUDE_DIRS}")
  else()
    message(WARNING "flutter_godot: webkit2gtk-4.1 not found. WebView support disabled.")
    message(WARNING "  Install with: sudo apt install libwebkit2gtk-4.1-dev")
  endif()
endif()

# List of absolute paths to libraries that should be bundled with the plugin
# webkit2gtk is a system library, no bundling needed
set(flutter_godot_bundled_libraries
  ""
  PARENT_SCOPE
)
