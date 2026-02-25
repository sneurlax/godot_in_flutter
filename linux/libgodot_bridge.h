// libgodot_bridge.h -- C bridge between Flutter and libgodot.so
//
// This thin C wrapper loads libgodot.so via dlopen and provides simple
// C functions that can be called from the Flutter native plugin. It handles:
// 1. Loading the libgodot shared library
// 2. Creating and managing a GodotInstance
// 3. Running the game loop (start/iteration/stop)
// 4. Thread management (Godot runs on a dedicated thread)
// 5. OpenGL callback wiring for embedded rendering
//
// The key challenge: libgodot exposes `libgodot_create_godot_instance`
// and `libgodot_destroy_godot_instance` as plain C symbols, but the
// GodotInstance's start/iteration/stop methods are GDExtension-bound
// methods that require the GDExtension method-call machinery.
//
// Solution: We register a minimal GDExtension that captures the
// getProcAddress function, then use it to call methods on the
// GodotInstance object.

#ifndef FLUTTER_GODOT_LIBGODOT_BRIDGE_H
#define FLUTTER_GODOT_LIBGODOT_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Status codes
typedef enum {
  GODOT_BRIDGE_OK = 0,
  GODOT_BRIDGE_ERROR_LIBRARY_NOT_FOUND = -1,
  GODOT_BRIDGE_ERROR_SYMBOL_NOT_FOUND = -2,
  GODOT_BRIDGE_ERROR_INSTANCE_CREATION_FAILED = -3,
  GODOT_BRIDGE_ERROR_ALREADY_INITIALIZED = -4,
  GODOT_BRIDGE_ERROR_NOT_INITIALIZED = -5,
  GODOT_BRIDGE_ERROR_THREAD_FAILED = -6,
} GodotBridgeStatus;

/// Callback invoked after each Godot frame iteration.
/// The host can use this to swap buffers and mark texture frame available.
typedef void (*GodotBridgeFrameCallback)(void* userdata);

/// OpenGL callbacks for embedded rendering.
/// These are called by Godot's GLManagerExternal to make the shared GL
/// context current / release it.
typedef void (*GodotBridgeMakeCurrentCallback)(void* userdata);
typedef void (*GodotBridgeDoneCurrentCallback)(void* userdata);

/// Initialize the bridge by loading libgodot.so from the given path.
/// Returns GODOT_BRIDGE_OK on success, or an error code.
GodotBridgeStatus godot_bridge_init(const char* library_path);

/// Create a Godot instance and start it with the given arguments.
/// The engine runs on a separate thread. The game loop (iteration())
/// is called automatically until the game quits or stop is called.
///
/// @param project_path  Path to the Godot project directory
/// @param pck_path      Path to the .pck file (NULL to use default)
/// @param rendering_method  "forward_plus", "mobile", or "gl_compatibility"
/// @param rendering_driver  "vulkan", "opengl3", etc.
/// @param extra_args    Additional command-line arguments (NULL-terminated array, or NULL)
/// @return GODOT_BRIDGE_OK on success
GodotBridgeStatus godot_bridge_start(
    const char* project_path,
    const char* pck_path,
    const char* rendering_method,
    const char* rendering_driver,
    const char** extra_args);

/// Set a callback to be invoked after each Godot frame.
void godot_bridge_set_frame_callback(GodotBridgeFrameCallback callback,
                                     void* userdata);

/// Set OpenGL context callbacks for embedded rendering.
/// These are used when Godot runs with --display-driver embedded
/// and --rendering-driver opengl3.
void godot_bridge_set_gl_callbacks(
    GodotBridgeMakeCurrentCallback make_current,
    GodotBridgeDoneCurrentCallback done_current,
    void* userdata);

/// Check if the Godot engine is currently running.
bool godot_bridge_is_running(void);

/// Request the Godot engine to stop.
/// This is thread-safe and can be called from any thread.
void godot_bridge_stop(void);

/// Wait for the Godot engine thread to finish and clean up.
/// Call this after godot_bridge_stop().
/// Returns GODOT_BRIDGE_OK on success.
GodotBridgeStatus godot_bridge_cleanup(void);

/// Shut down the bridge and unload the library.
void godot_bridge_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // FLUTTER_GODOT_LIBGODOT_BRIDGE_H
