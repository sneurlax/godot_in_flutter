#ifndef FLUTTER_PLUGIN_FLUTTER_GODOT_PLUGIN_H_
#define FLUTTER_PLUGIN_FLUTTER_GODOT_PLUGIN_H_

#include <flutter_linux/flutter_linux.h>

G_BEGIN_DECLS

#define FLUTTER_PLUGIN_EXPORT __attribute__((visibility("default")))

typedef struct _FlutterGodotPlugin FlutterGodotPlugin;
typedef struct {
  GObjectClass parent_class;
} FlutterGodotPluginClass;

#define FLUTTER_TYPE_GODOT_PLUGIN flutter_godot_plugin_get_type()
#define FLUTTER_GODOT_PLUGIN(obj)                     \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), FLUTTER_TYPE_GODOT_PLUGIN, \
                              FlutterGodotPlugin))

FLUTTER_PLUGIN_EXPORT GType flutter_godot_plugin_get_type();

// Plugin instance factory
FLUTTER_PLUGIN_EXPORT FlutterGodotPlugin* flutter_godot_plugin_new();

// This function MUST be defined for Flutter to load the plugin
FLUTTER_PLUGIN_EXPORT void
flutter_godot_plugin_register_with_registrar(FlPluginRegistrar* registrar);

G_END_DECLS

#endif  // FLUTTER_PLUGIN_FLUTTER_GODOT_PLUGIN_H_
