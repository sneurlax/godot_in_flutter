#include "flutter_godot_plugin.h"

#include <flutter_linux/flutter_linux.h>

struct _FlutterGodotPlugin {
  GObject parent_instance;
};

G_DEFINE_TYPE(FlutterGodotPlugin, flutter_godot_plugin, G_TYPE_OBJECT)

static void flutter_godot_plugin_class_init(FlutterGodotPluginClass* klass) {
}

static void flutter_godot_plugin_init(FlutterGodotPlugin* self) {
}

FlutterGodotPlugin* flutter_godot_plugin_new() {
  return FLUTTER_GODOT_PLUGIN(g_object_new(flutter_godot_plugin_get_type(), nullptr));
}

void flutter_godot_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  FlutterGodotPlugin* plugin = flutter_godot_plugin_new();
  g_object_unref(plugin);
}
