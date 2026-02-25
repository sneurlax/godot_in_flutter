/// flutter_godot
library;

import 'dart:async';
import 'dart:io' show Platform;

import 'package:flutter/widgets.dart';

import 'src/android.dart';
import 'src/linux.dart';
import 'src/platform_interface.dart';
import 'src/listen_callback.dart';
import 'src/unsupported.dart';

/// Export the callback for listening to data
export 'src/listen_callback.dart';

/// Export platform implementations for plugin registration
export 'src/linux.dart' show FlutterGodotLinux;
export 'src/linux_native.dart' show FlutterGodotLinuxNative;
export 'src/android.dart' show FlutterGodotAndroid;

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

  /// Forward input events to Godot
  static Future<void> forwardInputEvent({
    required double x,
    required double y,
    required String type,
    required int button,
    double pressure = 1.0,
  }) {
    return FlutterGodotPlatform.instance.forwardInputEvent(
      x: x,
      y: y,
      type: type,
      button: button,
      pressure: pressure,
    );
  }

  /// Game player widget
  static Widget ofPlayer({String? name, String? package}) {
    return FlutterGodotPlatform.instance.ofPlayer(name: name, package: package);
  }
}
