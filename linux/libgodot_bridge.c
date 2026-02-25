// libgodot_bridge.c -- C bridge between Flutter (native plugin) and libgodot.so
//
// This implements the bridge that loads libgodot.so at runtime, creates a
// GodotInstance, and runs the game loop on a separate thread.
//
// Architecture:
//   Flutter native plugin ---> libgodot_bridge --dlopen--> libgodot.so
//
// The bridge captures the GDExtension getProcAddress during initialization,
// then uses it to call GodotInstance methods (start, iteration, stop).
//
// For embedded OpenGL rendering, the bridge accepts make_current/done_current
// callbacks that Godot's GLManagerExternal uses to activate the shared GL context.

#include "libgodot_bridge.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>

// ============================================================================
// GDExtension types (minimal subset from gdextension_interface.h)
// ============================================================================

typedef void* GDExtensionObjectPtr;
typedef uint8_t GDExtensionBool;
typedef void* GDExtensionClassLibraryPtr;
typedef int32_t GDExtensionInitializationLevel;
typedef void (*GDExtensionInterfaceFunctionPtr)(void);
typedef GDExtensionInterfaceFunctionPtr (*GDExtensionInterfaceGetProcAddressFunc)(const char*);

typedef struct {
    GDExtensionInitializationLevel minimum_initialization_level;
    void* userdata;
    void (*initialize)(void* userdata, GDExtensionInitializationLevel level);
    void (*deinitialize)(void* userdata, GDExtensionInitializationLevel level);
} GDExtensionInitialization;

typedef GDExtensionBool (*GDExtensionInitializationFunction)(
    GDExtensionInterfaceGetProcAddressFunc p_get_proc_address,
    GDExtensionClassLibraryPtr p_library,
    GDExtensionInitialization* r_initialization);

// libgodot API function types
typedef GDExtensionObjectPtr (*CreateGodotInstanceFunc)(
    int argc, char* argv[], GDExtensionInitializationFunction init_func);
typedef void (*DestroyGodotInstanceFunc)(GDExtensionObjectPtr instance);

// ============================================================================
// GDExtension method call types (for calling start/iteration on GodotInstance)
// ============================================================================

typedef void* GDExtensionMethodBindPtr;
typedef void* GDExtensionStringNamePtr;

typedef void (*ObjectMethodBindCallFunc)(
    GDExtensionMethodBindPtr p_method_bind,
    GDExtensionObjectPtr p_instance,
    const void* const* p_args,
    int64_t p_arg_count,
    void* r_return,
    void* r_error);

typedef GDExtensionMethodBindPtr (*ClassDBGetMethodBindFunc)(
    GDExtensionStringNamePtr p_classname,
    GDExtensionStringNamePtr p_methodname,
    int64_t p_hash);

typedef void (*StringNameNewWithUtf8CharsFunc)(
    GDExtensionStringNamePtr r_dest,
    const char* p_contents);

typedef void (*StringNameDestroyFunc)(GDExtensionStringNamePtr p_self);

// ============================================================================
// Bridge state
// ============================================================================

static void* lib_handle = NULL;
static CreateGodotInstanceFunc create_instance_func = NULL;
static DestroyGodotInstanceFunc destroy_instance_func = NULL;
static GDExtensionObjectPtr godot_instance = NULL;
static pthread_t godot_thread;
static atomic_bool godot_running = false;
static atomic_bool godot_stop_requested = false;
static bool bridge_initialized = false;

// Captured GDExtension API
static GDExtensionInterfaceGetProcAddressFunc captured_get_proc_address = NULL;
static ObjectMethodBindCallFunc object_method_bind_call = NULL;
static ClassDBGetMethodBindFunc classdb_get_method_bind = NULL;
static StringNameNewWithUtf8CharsFunc string_name_new = NULL;
static StringNameDestroyFunc string_name_destroy = NULL;

// Method binds (cached)
static GDExtensionMethodBindPtr mb_start = NULL;
static GDExtensionMethodBindPtr mb_iteration = NULL;
static GDExtensionMethodBindPtr mb_is_started = NULL;

// Frame callback
static GodotBridgeFrameCallback frame_callback = NULL;
static void* frame_callback_userdata = NULL;

// GL callbacks for embedded rendering
static GodotBridgeMakeCurrentCallback gl_make_current_cb = NULL;
static GodotBridgeDoneCurrentCallback gl_done_current_cb = NULL;
static void* gl_callback_userdata = NULL;

// ============================================================================
// GDExtension initialization callback
// ============================================================================

static void bridge_extension_initialize(void* userdata, GDExtensionInitializationLevel level) {
    (void)userdata;
    if (level != 2) return; // SCENE level = 2

    fprintf(stderr, "[godot_bridge] Extension initialized at SCENE level\n");

    if (captured_get_proc_address) {
        object_method_bind_call = (ObjectMethodBindCallFunc)(void*)
            captured_get_proc_address("object_method_bind_call");
        classdb_get_method_bind = (ClassDBGetMethodBindFunc)(void*)
            captured_get_proc_address("classdb_get_method_bind");
        string_name_new = (StringNameNewWithUtf8CharsFunc)(void*)
            captured_get_proc_address("string_name_new_with_utf8_chars");
        string_name_destroy = (StringNameDestroyFunc)(void*)
            captured_get_proc_address("string_name_destroy");

        if (classdb_get_method_bind && string_name_new && string_name_destroy) {
            char classname_buf[64] = {0};
            char methodname_buf[64] = {0};

            // Look up GodotInstance.start()
            string_name_new((GDExtensionStringNamePtr)classname_buf, "GodotInstance");
            string_name_new((GDExtensionStringNamePtr)methodname_buf, "start");
            mb_start = classdb_get_method_bind(
                (GDExtensionStringNamePtr)classname_buf,
                (GDExtensionStringNamePtr)methodname_buf,
                0);
            string_name_destroy((GDExtensionStringNamePtr)methodname_buf);

            // Look up GodotInstance.iteration()
            memset(methodname_buf, 0, sizeof(methodname_buf));
            string_name_new((GDExtensionStringNamePtr)methodname_buf, "iteration");
            mb_iteration = classdb_get_method_bind(
                (GDExtensionStringNamePtr)classname_buf,
                (GDExtensionStringNamePtr)methodname_buf,
                0);
            string_name_destroy((GDExtensionStringNamePtr)methodname_buf);

            // Look up GodotInstance.is_started()
            memset(methodname_buf, 0, sizeof(methodname_buf));
            string_name_new((GDExtensionStringNamePtr)methodname_buf, "is_started");
            mb_is_started = classdb_get_method_bind(
                (GDExtensionStringNamePtr)classname_buf,
                (GDExtensionStringNamePtr)methodname_buf,
                0);

            string_name_destroy((GDExtensionStringNamePtr)classname_buf);
            string_name_destroy((GDExtensionStringNamePtr)methodname_buf);

            fprintf(stderr, "[godot_bridge] Method binds: start=%p, iteration=%p, is_started=%p\n",
                    mb_start, mb_iteration, mb_is_started);
        } else {
            fprintf(stderr, "[godot_bridge] WARNING: Could not find required GDExtension API functions\n");
        }
    }
}

static void bridge_extension_deinitialize(void* userdata, GDExtensionInitializationLevel level) {
    (void)userdata;
    (void)level;
}

static GDExtensionBool bridge_extension_init(
    GDExtensionInterfaceGetProcAddressFunc p_get_proc_address,
    GDExtensionClassLibraryPtr p_library,
    GDExtensionInitialization* r_initialization) {
    (void)p_library;

    captured_get_proc_address = p_get_proc_address;

    r_initialization->minimum_initialization_level = 2; // SCENE
    r_initialization->userdata = NULL;
    r_initialization->initialize = bridge_extension_initialize;
    r_initialization->deinitialize = bridge_extension_deinitialize;

    fprintf(stderr, "[godot_bridge] GDExtension init callback registered\n");
    return 1;
}

// ============================================================================
// Helper: Call a method on GodotInstance via GDExtension
// ============================================================================

static bool call_godot_method_bool(GDExtensionMethodBindPtr method_bind) {
    if (!method_bind || !object_method_bind_call || !godot_instance) {
        return false;
    }

    char return_value[32] = {0};
    char call_error[32] = {0};

    object_method_bind_call(
        method_bind,
        godot_instance,
        NULL,
        0,
        return_value,
        call_error);

    int64_t variant_type = *(int64_t*)return_value;
    if (variant_type == 1) { // BOOL
        return *(GDExtensionBool*)(return_value + 8) != 0;
    }

    return false;
}

// ============================================================================
// Godot engine thread
// ============================================================================

static void* godot_thread_func(void* arg) {
    (void)arg;
    fprintf(stderr, "[godot_bridge] Godot thread started\n");

    // Call start() on the GodotInstance
    if (mb_start) {
        bool started = call_godot_method_bool(mb_start);
        fprintf(stderr, "[godot_bridge] GodotInstance.start() returned: %d\n", started);
    } else {
        fprintf(stderr, "[godot_bridge] WARNING: mb_start is NULL, cannot call start()\n");
    }

    // Game loop
    atomic_store(&godot_running, true);

    // Target ~60 FPS
    struct timespec frame_time = {0, 16666667}; // ~16.67ms

    while (!atomic_load(&godot_stop_requested)) {
        if (mb_iteration) {
            bool should_quit = call_godot_method_bool(mb_iteration);
            if (should_quit) {
                fprintf(stderr, "[godot_bridge] Godot requested quit\n");
                break;
            }

            // Invoke frame callback so the host can swap buffers / mark texture
            if (frame_callback) {
                frame_callback(frame_callback_userdata);
            }
        } else {
            fprintf(stderr, "[godot_bridge] WARNING: mb_iteration is NULL\n");
            break;
        }

        // Sleep to target ~60 FPS
        nanosleep(&frame_time, NULL);
    }

    atomic_store(&godot_running, false);
    fprintf(stderr, "[godot_bridge] Godot thread finished\n");
    return NULL;
}

// ============================================================================
// Public API implementation
// ============================================================================

GodotBridgeStatus godot_bridge_init(const char* library_path) {
    if (bridge_initialized) {
        return GODOT_BRIDGE_ERROR_ALREADY_INITIALIZED;
    }

    fprintf(stderr, "[godot_bridge] Loading libgodot from: %s\n", library_path);

    lib_handle = dlopen(library_path, RTLD_LAZY | RTLD_GLOBAL);
    if (!lib_handle) {
        fprintf(stderr, "[godot_bridge] ERROR: dlopen failed: %s\n", dlerror());
        return GODOT_BRIDGE_ERROR_LIBRARY_NOT_FOUND;
    }

    create_instance_func = (CreateGodotInstanceFunc)dlsym(lib_handle, "libgodot_create_godot_instance");
    if (!create_instance_func) {
        fprintf(stderr, "[godot_bridge] ERROR: Symbol not found: %s\n", dlerror());
        dlclose(lib_handle);
        lib_handle = NULL;
        return GODOT_BRIDGE_ERROR_SYMBOL_NOT_FOUND;
    }

    destroy_instance_func = (DestroyGodotInstanceFunc)dlsym(lib_handle, "libgodot_destroy_godot_instance");
    if (!destroy_instance_func) {
        fprintf(stderr, "[godot_bridge] ERROR: Symbol not found: %s\n", dlerror());
        dlclose(lib_handle);
        lib_handle = NULL;
        return GODOT_BRIDGE_ERROR_SYMBOL_NOT_FOUND;
    }

    bridge_initialized = true;
    fprintf(stderr, "[godot_bridge] Library loaded successfully\n");
    return GODOT_BRIDGE_OK;
}

GodotBridgeStatus godot_bridge_start(
    const char* project_path,
    const char* pck_path,
    const char* rendering_method,
    const char* rendering_driver,
    const char** extra_args) {

    if (!bridge_initialized) {
        return GODOT_BRIDGE_ERROR_NOT_INITIALIZED;
    }

    if (godot_instance) {
        return GODOT_BRIDGE_ERROR_ALREADY_INITIALIZED;
    }

    // Build argv
    char* argv[32];
    int argc = 0;

    argv[argc++] = "flutter_godot";

    if (project_path) {
        argv[argc++] = "--path";
        argv[argc++] = (char*)project_path;
    }

    if (pck_path) {
        argv[argc++] = "--main-pack";
        argv[argc++] = (char*)pck_path;
    }

    if (rendering_method) {
        argv[argc++] = "--rendering-method";
        argv[argc++] = (char*)rendering_method;
    }

    if (rendering_driver) {
        argv[argc++] = "--rendering-driver";
        argv[argc++] = (char*)rendering_driver;
    }

    // Add extra args
    if (extra_args) {
        for (int i = 0; extra_args[i] && argc < 30; i++) {
            argv[argc++] = (char*)extra_args[i];
        }
    }

    argv[argc] = NULL;

    fprintf(stderr, "[godot_bridge] Creating Godot instance with %d args:\n", argc);
    for (int i = 0; i < argc; i++) {
        fprintf(stderr, "  argv[%d] = %s\n", i, argv[i]);
    }

    // Create the Godot instance
    godot_instance = create_instance_func(argc, argv, bridge_extension_init);
    if (!godot_instance) {
        fprintf(stderr, "[godot_bridge] ERROR: Failed to create Godot instance\n");
        return GODOT_BRIDGE_ERROR_INSTANCE_CREATION_FAILED;
    }

    fprintf(stderr, "[godot_bridge] Godot instance created at %p\n", godot_instance);

    // Start the game loop on a separate thread
    atomic_store(&godot_stop_requested, false);

    int ret = pthread_create(&godot_thread, NULL, godot_thread_func, NULL);
    if (ret != 0) {
        fprintf(stderr, "[godot_bridge] ERROR: pthread_create failed: %d\n", ret);
        destroy_instance_func(godot_instance);
        godot_instance = NULL;
        return GODOT_BRIDGE_ERROR_THREAD_FAILED;
    }

    fprintf(stderr, "[godot_bridge] Godot thread launched\n");
    return GODOT_BRIDGE_OK;
}

void godot_bridge_set_frame_callback(GodotBridgeFrameCallback callback,
                                     void* userdata) {
    frame_callback = callback;
    frame_callback_userdata = userdata;
}

void godot_bridge_set_gl_callbacks(
    GodotBridgeMakeCurrentCallback make_current,
    GodotBridgeDoneCurrentCallback done_current,
    void* userdata) {
    gl_make_current_cb = make_current;
    gl_done_current_cb = done_current;
    gl_callback_userdata = userdata;
    fprintf(stderr, "[godot_bridge] GL callbacks set: make_current=%p, done_current=%p\n",
            (void*)(intptr_t)make_current, (void*)(intptr_t)done_current);
}

bool godot_bridge_is_running(void) {
    return atomic_load(&godot_running);
}

void godot_bridge_stop(void) {
    atomic_store(&godot_stop_requested, true);
}

GodotBridgeStatus godot_bridge_cleanup(void) {
    if (!godot_instance) {
        return GODOT_BRIDGE_ERROR_NOT_INITIALIZED;
    }

    godot_bridge_stop();
    pthread_join(godot_thread, NULL);

    destroy_instance_func(godot_instance);
    godot_instance = NULL;

    fprintf(stderr, "[godot_bridge] Cleanup complete\n");
    return GODOT_BRIDGE_OK;
}

void godot_bridge_shutdown(void) {
    if (godot_instance) {
        godot_bridge_cleanup();
    }

    if (lib_handle) {
        dlclose(lib_handle);
        lib_handle = NULL;
    }

    bridge_initialized = false;
    captured_get_proc_address = NULL;
    object_method_bind_call = NULL;
    classdb_get_method_bind = NULL;
    string_name_new = NULL;
    string_name_destroy = NULL;
    mb_start = NULL;
    mb_iteration = NULL;
    mb_is_started = NULL;
    frame_callback = NULL;
    frame_callback_userdata = NULL;
    gl_make_current_cb = NULL;
    gl_done_current_cb = NULL;
    gl_callback_userdata = NULL;

    fprintf(stderr, "[godot_bridge] Shutdown complete\n");
}
