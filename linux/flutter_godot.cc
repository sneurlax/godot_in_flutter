// flutter_godot.cc -- Native Flutter plugin for Linux
//
// This is the native Godot-in-Flutter embedded rendering integration on Linux
// using LibGodot + FlTextureGL with shared GDK GL context.
//
// Architecture:
//   1. Dart calls "initializeNative" via method channel
//   2. C plugin creates a shared GDK GL context from Flutter's window
//   3. Creates double-buffered FBOs in the shared context
//   4. Creates GodotGLTexture (FlTextureGL subclass), registers with Flutter
//   5. Loads libgodot.so via bridge, starts Godot with --display-driver embedded
//   6. Godot's GLManagerExternal uses our FBO as its render target
//   7. After each frame: swap buffers + mark Flutter texture updated
//   8. Flutter's raster thread reads the front texture via populate()
//   9. Dart displays via Texture(textureId: ...) widget
//
// The key insight: GDK GL contexts created from the same GdkWindow share
// all GL objects (textures, FBOs, renderbuffers). So Godot's color texture
// is directly readable by Flutter's raster thread -- zero copy.

#include "flutter_godot_plugin.h"
#include "godot_gl_texture.h"
#include "libgodot_bridge.h"

#include <flutter_linux/flutter_linux.h>
#include <gdk/gdk.h>
#include <epoxy/gl.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

// Forward declarations
static void method_call_handler(FlMethodChannel* channel,
                                FlMethodCall* method_call,
                                gpointer user_data);

// Plugin state
struct _FlutterGodotPlugin {
  GObject parent_instance;

  FlMethodChannel* method_channel;
  FlTextureRegistrar* texture_registrar;
  FlPluginRegistrar* registrar;

  // OpenGL texture for rendering (double-buffered)
  GodotGLTexture* godot_texture;
  int64_t texture_id;

  // Whether Godot engine is initialized (native mode)
  gboolean godot_initialized;

  // Whether we're using native embedded mode vs WebView fallback
  gboolean native_mode;
};

G_DEFINE_TYPE(FlutterGodotPlugin, flutter_godot_plugin, G_TYPE_OBJECT)

// ============================================================================
// GL context callbacks for Godot's GLManagerExternal
// ============================================================================

// Called by Godot when it needs to make the GL context current for rendering.
// This runs on Godot's rendering thread.
static void on_godot_make_current(void* userdata) {
  GodotGLTexture* tex = (GodotGLTexture*)userdata;
  GdkGLContext* ctx = godot_gl_texture_get_context(tex);
  if (ctx) {
    gdk_gl_context_make_current(ctx);
  }
}

// Called by Godot when it's done with the GL context.
static void on_godot_done_current(void* userdata) {
  (void)userdata;
  gdk_gl_context_clear_current();
}

// ============================================================================
// Frame callback: called after each Godot iteration
// ============================================================================

// This is called from the Godot thread. We swap the double buffers
// and then schedule a texture update on the main thread.
typedef struct {
  FlutterGodotPlugin* plugin;
} FrameNotifyData;

static gboolean notify_frame_on_main_thread(gpointer user_data) {
  FrameNotifyData* data = (FrameNotifyData*)user_data;
  if (data->plugin && data->plugin->texture_registrar && data->plugin->godot_texture) {
    fl_texture_registrar_mark_texture_frame_available(
        data->plugin->texture_registrar,
        FL_TEXTURE(data->plugin->godot_texture));
  }
  g_free(data);
  return G_SOURCE_REMOVE;  // One-shot
}

static void on_godot_frame_complete(void* userdata) {
  FlutterGodotPlugin* plugin = (FlutterGodotPlugin*)userdata;
  if (!plugin || !plugin->godot_texture) return;

  // Swap front/back buffers (thread-safe via mutex)
  godot_gl_texture_swap_buffers(plugin->godot_texture);

  // Schedule texture update notification on the main thread
  // (fl_texture_registrar_mark_texture_frame_available must be called
  //  from the main/platform thread)
  FrameNotifyData* data = g_new0(FrameNotifyData, 1);
  data->plugin = plugin;
  g_idle_add(notify_frame_on_main_thread, data);
}

// ============================================================================
// Method channel handlers
// ============================================================================

static void handle_initialize_native(FlutterGodotPlugin* plugin,
                                     FlMethodCall* method_call) {
  FlValue* args = fl_method_call_get_args(method_call);
  FlMethodResponse* response = nullptr;

  if (plugin->godot_initialized) {
    // Return existing texture ID
    g_autoptr(FlValue) result = fl_value_new_map();
    fl_value_set_string_take(result, "textureId",
                             fl_value_new_int(plugin->texture_id));
    fl_value_set_string_take(result, "status",
                             fl_value_new_string("already_initialized"));
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
    return;
  }

  // Extract parameters
  const char* libgodot_path = nullptr;
  const char* pck_path = nullptr;
  int width = 800;
  int height = 600;

  if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
    FlValue* v;
    v = fl_value_lookup_string(args, "libgodotPath");
    if (v && fl_value_get_type(v) == FL_VALUE_TYPE_STRING)
      libgodot_path = fl_value_get_string(v);

    v = fl_value_lookup_string(args, "pckPath");
    if (v && fl_value_get_type(v) == FL_VALUE_TYPE_STRING)
      pck_path = fl_value_get_string(v);

    v = fl_value_lookup_string(args, "width");
    if (v && fl_value_get_type(v) == FL_VALUE_TYPE_INT)
      width = (int)fl_value_get_int(v);

    v = fl_value_lookup_string(args, "height");
    if (v && fl_value_get_type(v) == FL_VALUE_TYPE_INT)
      height = (int)fl_value_get_int(v);
  }

  // Default libgodot path: check known locations
  if (!libgodot_path) {
    const char* env_path = getenv("LIBGODOT_PATH");
    if (env_path && access(env_path, R_OK) == 0) {
      libgodot_path = env_path;
    }
  }
  if (!libgodot_path) {
    // Try the default build location
    const char* default_paths[] = {
      "/tmp/libgodot-migeran/godot/bin/libgodot.linuxbsd.template_release.x86_64.so",
      NULL
    };
    for (int i = 0; default_paths[i]; i++) {
      if (access(default_paths[i], R_OK) == 0) {
        libgodot_path = default_paths[i];
        break;
      }
    }
  }

  if (!libgodot_path) {
    g_autoptr(FlValue) err_detail = fl_value_new_string(
        "libgodot.so not found. Pass libgodotPath, set LIBGODOT_PATH env, "
        "or place it at /tmp/libgodot-migeran/godot/bin/");
    response = FL_METHOD_RESPONSE(fl_method_error_response_new(
        "LIBGODOT_NOT_FOUND", "Cannot find libgodot.so", err_detail));
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
    return;
  }

  fprintf(stderr, "[flutter_godot] Initializing native Godot embedding...\n");
  fprintf(stderr, "[flutter_godot] libgodot path: %s\n", libgodot_path);
  fprintf(stderr, "[flutter_godot] Texture size: %dx%d\n", width, height);

  // Step 1: Get FlView and create a shared GDK GL context
  FlView* fl_view = fl_plugin_registrar_get_view(plugin->registrar);
  if (!fl_view) {
    g_autoptr(FlValue) err_detail = fl_value_new_string("Cannot get FlView from registrar");
    response = FL_METHOD_RESPONSE(fl_method_error_response_new(
        "VIEW_ERROR", "FlView not available", err_detail));
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
    return;
  }

  GdkWindow* gdk_window = gtk_widget_get_parent_window(GTK_WIDGET(fl_view));
  if (!gdk_window) {
    g_autoptr(FlValue) err_detail = fl_value_new_string("Cannot get GdkWindow from FlView");
    response = FL_METHOD_RESPONSE(fl_method_error_response_new(
        "WINDOW_ERROR", "GdkWindow not available", err_detail));
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
    return;
  }

  GError* gl_error = NULL;
  GdkGLContext* shared_context = gdk_window_create_gl_context(gdk_window, &gl_error);
  if (!shared_context) {
    char err_msg[512];
    snprintf(err_msg, sizeof(err_msg), "Failed to create GDK GL context: %s",
             gl_error ? gl_error->message : "unknown error");
    if (gl_error) g_error_free(gl_error);

    g_autoptr(FlValue) err_detail = fl_value_new_string(err_msg);
    response = FL_METHOD_RESPONSE(fl_method_error_response_new(
        "GL_ERROR", err_msg, err_detail));
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
    return;
  }

  fprintf(stderr, "[flutter_godot] Created shared GDK GL context: %p\n", (void*)shared_context);

  // Step 2: Create the GodotGLTexture (FlTextureGL subclass)
  plugin->godot_texture = godot_gl_texture_new(shared_context, width, height);
  g_object_unref(shared_context);  // texture takes its own ref

  if (!plugin->godot_texture) {
    g_autoptr(FlValue) err_detail = fl_value_new_string("Failed to create GodotGLTexture");
    response = FL_METHOD_RESPONSE(fl_method_error_response_new(
        "TEXTURE_ERROR", "Cannot create texture", err_detail));
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
    return;
  }

  // Step 3: Initialize GL resources (FBOs, textures)
  if (!godot_gl_texture_init_gl(plugin->godot_texture)) {
    g_object_unref(plugin->godot_texture);
    plugin->godot_texture = nullptr;
    g_autoptr(FlValue) err_detail = fl_value_new_string("Failed to initialize GL resources");
    response = FL_METHOD_RESPONSE(fl_method_error_response_new(
        "GL_ERROR", "GL initialization failed", err_detail));
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
    return;
  }

  // Step 4: Register the texture with Flutter
  gboolean registered = fl_texture_registrar_register_texture(
      plugin->texture_registrar,
      FL_TEXTURE(plugin->godot_texture));

  if (!registered) {
    fprintf(stderr, "[flutter_godot] ERROR: Failed to register texture\n");
    g_object_unref(plugin->godot_texture);
    plugin->godot_texture = nullptr;
    g_autoptr(FlValue) err_detail = fl_value_new_string("Failed to register texture with Flutter");
    response = FL_METHOD_RESPONSE(fl_method_error_response_new(
        "TEXTURE_ERROR", "Cannot register texture", err_detail));
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
    return;
  }

  plugin->texture_id = fl_texture_get_id(FL_TEXTURE(plugin->godot_texture));
  fprintf(stderr, "[flutter_godot] Texture registered with ID: %ld\n",
          (long)plugin->texture_id);

  // Mark first frame available so the initial placeholder is shown
  fl_texture_registrar_mark_texture_frame_available(
      plugin->texture_registrar,
      FL_TEXTURE(plugin->godot_texture));

  // Step 5: Initialize the libgodot bridge
  GodotBridgeStatus status = godot_bridge_init(libgodot_path);
  if (status != GODOT_BRIDGE_OK) {
    fprintf(stderr, "[flutter_godot] ERROR: godot_bridge_init failed: %d\n", status);
    fl_texture_registrar_unregister_texture(
        plugin->texture_registrar, FL_TEXTURE(plugin->godot_texture));
    g_object_unref(plugin->godot_texture);
    plugin->godot_texture = nullptr;

    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg), "godot_bridge_init failed with code %d", status);
    g_autoptr(FlValue) err_detail = fl_value_new_string(err_msg);
    response = FL_METHOD_RESPONSE(fl_method_error_response_new(
        "BRIDGE_ERROR", err_msg, err_detail));
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
    return;
  }

  // Step 6: Set up GL callbacks for embedded rendering
  godot_bridge_set_gl_callbacks(
      on_godot_make_current,
      on_godot_done_current,
      plugin->godot_texture);

  // Step 7: Set frame callback (swap buffers + notify Flutter)
  godot_bridge_set_frame_callback(on_godot_frame_complete, plugin);

  // Step 8: Start the Godot engine with embedded display driver and OpenGL
  // Use --display-driver embedded so Godot doesn't open its own window.
  // If that fails (libgodot not built with EXTERNAL_TARGET_ENABLED),
  // we fall back to opening its own window.
  const char* extra_args[] = {
    "--display-driver", "embedded",
    "--verbose",
    NULL
  };

  status = godot_bridge_start(
      NULL,                    // project_path (use pck_path instead)
      pck_path,                // pck file
      "gl_compatibility",      // rendering_method
      "opengl3",               // rendering_driver
      extra_args);

  if (status != GODOT_BRIDGE_OK) {
    fprintf(stderr, "[flutter_godot] Embedded mode failed (status=%d), trying without --display-driver embedded...\n",
            status);

    // Try again without --display-driver embedded
    // This will open a separate Godot window, but at least it runs
    status = godot_bridge_start(
        NULL,
        pck_path,
        "gl_compatibility",
        "opengl3",
        NULL);
  }

  if (status != GODOT_BRIDGE_OK) {
    fprintf(stderr, "[flutter_godot] ERROR: godot_bridge_start failed: %d\n", status);
    godot_bridge_shutdown();
    fl_texture_registrar_unregister_texture(
        plugin->texture_registrar, FL_TEXTURE(plugin->godot_texture));
    g_object_unref(plugin->godot_texture);
    plugin->godot_texture = nullptr;

    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg), "godot_bridge_start failed with code %d", status);
    g_autoptr(FlValue) err_detail = fl_value_new_string(err_msg);
    response = FL_METHOD_RESPONSE(fl_method_error_response_new(
        "BRIDGE_ERROR", err_msg, err_detail));
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
    return;
  }

  fprintf(stderr, "[flutter_godot] Godot engine started successfully!\n");

  plugin->godot_initialized = TRUE;
  plugin->native_mode = TRUE;

  // Return success with texture ID
  g_autoptr(FlValue) result = fl_value_new_map();
  fl_value_set_string_take(result, "textureId",
                           fl_value_new_int(plugin->texture_id));
  fl_value_set_string_take(result, "status",
                           fl_value_new_string("initialized"));
  fl_value_set_string_take(result, "mode",
                           fl_value_new_string("native_gl"));
  fl_value_set_string_take(result, "width",
                           fl_value_new_int(width));
  fl_value_set_string_take(result, "height",
                           fl_value_new_int(height));
  response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));

  g_autoptr(GError) error = nullptr;
  fl_method_call_respond(method_call, response, &error);
  g_object_unref(response);
}

static void handle_stop_native(FlutterGodotPlugin* plugin,
                               FlMethodCall* method_call) {
  if (plugin->godot_initialized) {
    godot_bridge_stop();
    godot_bridge_cleanup();
    godot_bridge_shutdown();
    plugin->godot_initialized = FALSE;
    fprintf(stderr, "[flutter_godot] Godot engine stopped\n");
  }

  if (plugin->godot_texture) {
    fl_texture_registrar_unregister_texture(
        plugin->texture_registrar, FL_TEXTURE(plugin->godot_texture));
    g_object_unref(plugin->godot_texture);
    plugin->godot_texture = nullptr;
    plugin->texture_id = -1;
  }

  g_autoptr(FlValue) result = fl_value_new_bool(TRUE);
  FlMethodResponse* response =
      FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  g_autoptr(GError) error = nullptr;
  fl_method_call_respond(method_call, response, &error);
  g_object_unref(response);
}

static void handle_get_status(FlutterGodotPlugin* plugin,
                              FlMethodCall* method_call) {
  g_autoptr(FlValue) result = fl_value_new_map();
  fl_value_set_string_take(result, "initialized",
                           fl_value_new_bool(plugin->godot_initialized));
  fl_value_set_string_take(result, "running",
                           fl_value_new_bool(godot_bridge_is_running()));
  fl_value_set_string_take(result, "textureId",
                           fl_value_new_int(plugin->texture_id));
  fl_value_set_string_take(result, "nativeMode",
                           fl_value_new_bool(plugin->native_mode));

  FlMethodResponse* response =
      FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  g_autoptr(GError) error = nullptr;
  fl_method_call_respond(method_call, response, &error);
  g_object_unref(response);
}

// ============================================================================
// Method channel dispatch
// ============================================================================

static void method_call_handler(FlMethodChannel* channel,
                                FlMethodCall* method_call,
                                gpointer user_data) {
  FlutterGodotPlugin* plugin = FLUTTER_GODOT_PLUGIN(user_data);
  const gchar* method = fl_method_call_get_name(method_call);

  if (strcmp(method, "initializeNative") == 0) {
    handle_initialize_native(plugin, method_call);
  } else if (strcmp(method, "stopNative") == 0) {
    handle_stop_native(plugin, method_call);
  } else if (strcmp(method, "getStatus") == 0) {
    handle_get_status(plugin, method_call);
  } else if (strcmp(method, "getPlatformVersion") == 0) {
    g_autoptr(FlValue) result = fl_value_new_string("Linux (LibGodot native GL mode)");
    FlMethodResponse* response =
        FL_METHOD_RESPONSE(fl_method_success_response_new(result));
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
  } else {
    FlMethodResponse* response =
        FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
  }
}

// ============================================================================
// Plugin lifecycle
// ============================================================================

static void flutter_godot_plugin_dispose(GObject* object) {
  FlutterGodotPlugin* plugin = FLUTTER_GODOT_PLUGIN(object);

  // Stop Godot if running
  if (plugin->godot_initialized) {
    godot_bridge_stop();
    godot_bridge_cleanup();
    godot_bridge_shutdown();
    plugin->godot_initialized = FALSE;
  }

  if (plugin->godot_texture && plugin->texture_registrar) {
    fl_texture_registrar_unregister_texture(
        plugin->texture_registrar, FL_TEXTURE(plugin->godot_texture));
    g_clear_object(&plugin->godot_texture);
  }

  g_clear_object(&plugin->method_channel);

  G_OBJECT_CLASS(flutter_godot_plugin_parent_class)->dispose(object);
}

static void flutter_godot_plugin_class_init(FlutterGodotPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = flutter_godot_plugin_dispose;
}

static void flutter_godot_plugin_init(FlutterGodotPlugin* self) {
  self->method_channel = nullptr;
  self->texture_registrar = nullptr;
  self->registrar = nullptr;
  self->godot_texture = nullptr;
  self->texture_id = -1;
  self->godot_initialized = FALSE;
  self->native_mode = FALSE;
}

FlutterGodotPlugin* flutter_godot_plugin_new() {
  return FLUTTER_GODOT_PLUGIN(
      g_object_new(flutter_godot_plugin_get_type(), nullptr));
}

void flutter_godot_plugin_register_with_registrar(
    FlPluginRegistrar* registrar) {
  FlutterGodotPlugin* plugin = flutter_godot_plugin_new();

  plugin->registrar = registrar;
  plugin->texture_registrar =
      fl_plugin_registrar_get_texture_registrar(registrar);

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();

  plugin->method_channel = fl_method_channel_new(
      fl_plugin_registrar_get_messenger(registrar),
      "flutter_godot_method",
      FL_METHOD_CODEC(codec));

  fl_method_channel_set_method_call_handler(
      plugin->method_channel,
      method_call_handler,
      g_object_ref(plugin),
      g_object_unref);

  fprintf(stderr, "[flutter_godot] Plugin registered (LibGodot native GL mode)\n");

  g_object_unref(plugin);
}
