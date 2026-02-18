/// flutter_godot
library;

import 'dart:async';

import 'package:flutter/widgets.dart';

import 'src/android.dart';
import 'src/platform_interface.dart';
import 'src/listen_callback.dart';
import 'src/unsupported.dart';

/// Export the callback for listening to data
export 'src/listen_callback.dart';

/// Compatibility handling for unsupported platforms
part 'flutter_godot_compat.dart';

final class FlutterGodot {
  const FlutterGodot._();

  /// Register the flutter_godot plugin.
  /// Plugin registration is managed by the Flutter framework, please do not register manually.
  static void registerWith() {
    FlutterGodotPlatform.instance = FlutterGodotAndroid();
  }

  /// Send data to Godot
  static Future<bool> sendDataToGodot({required String data}) {
    return FlutterGodotPlatform.instance.sendDataToGodot(data: data);
  }

  /// Listen for data sent from Godot
  static StreamSubscription<dynamic> listenGodotData({
    required GodotListenCallback callback,
  }) {
    return FlutterGodotPlatform.instance.listenGodotData(callback: callback);
  }

  /// Game player widget
  static Widget ofPlayer({String? name, String? package}) {
    return FlutterGodotPlatform.instance.ofPlayer(name: name, package: package);
  }
}
