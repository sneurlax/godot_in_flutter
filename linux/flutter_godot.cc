// flutter_godot.cc -- Minimal native plugin for Linux
//
// The WebView-based approach handles everything in Dart using an HTTP server
// and console-based IPC. This native plugin just registers with Flutter.

#include "flutter_godot_plugin.h"

#include <flutter_linux/flutter_linux.h>

#include <cstring>

// Forward declarations
static void method_call_handler(FlMethodChannel* channel,
                                FlMethodCall* method_call,
                                gpointer user_data);

// Plugin state
struct _FlutterGodotPlugin {
  GObject parent_instance;
  FlMethodChannel* method_channel;
};

G_DEFINE_TYPE(FlutterGodotPlugin, flutter_godot_plugin, G_TYPE_OBJECT)

// Method channel dispatch
static void method_call_handler(FlMethodChannel* channel,
                                FlMethodCall* method_call,
                                gpointer user_data) {
  const gchar* method = fl_method_call_get_name(method_call);

  FlMethodResponse* response = nullptr;

  // For the WebView approach, all methods are handled in Dart.
  // Return success for known methods to avoid errors.
  if (strcmp(method, "getPlatformVersion") == 0) {
    g_autoptr(FlValue) result = fl_value_new_string("Linux (WebView mode)");
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  g_autoptr(GError) error = nullptr;
  fl_method_call_respond(method_call, response, &error);
  g_object_unref(response);
}

// Plugin lifecycle
static void flutter_godot_plugin_dispose(GObject* object) {
  FlutterGodotPlugin* plugin = FLUTTER_GODOT_PLUGIN(object);
  g_clear_object(&plugin->method_channel);
  G_OBJECT_CLASS(flutter_godot_plugin_parent_class)->dispose(object);
}

static void flutter_godot_plugin_class_init(FlutterGodotPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = flutter_godot_plugin_dispose;
}

static void flutter_godot_plugin_init(FlutterGodotPlugin* self) {
  self->method_channel = nullptr;
}

FlutterGodotPlugin* flutter_godot_plugin_new() {
  return FLUTTER_GODOT_PLUGIN(
      g_object_new(flutter_godot_plugin_get_type(), nullptr));
}

void flutter_godot_plugin_register_with_registrar(
    FlPluginRegistrar* registrar) {
  FlutterGodotPlugin* plugin = flutter_godot_plugin_new();

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

  fprintf(stderr, "[flutter_godot] Plugin registered (WebView mode)\n");

  g_object_unref(plugin);
}
