//
//  Generated file. Do not edit.
//

// clang-format off

#include "generated_plugin_registrant.h"

#include <flutter_godot/flutter_godot_plugin.h>
#include <webview_cef/webview_cef_plugin.h>

void fl_register_plugins(FlPluginRegistry* registry) {
  g_autoptr(FlPluginRegistrar) flutter_godot_registrar =
      fl_plugin_registry_get_registrar_for_plugin(registry, "FlutterGodotPlugin");
  flutter_godot_plugin_register_with_registrar(flutter_godot_registrar);
  g_autoptr(FlPluginRegistrar) webview_cef_registrar =
      fl_plugin_registry_get_registrar_for_plugin(registry, "WebviewCefPlugin");
  webview_cef_plugin_register_with_registrar(webview_cef_registrar);
}
