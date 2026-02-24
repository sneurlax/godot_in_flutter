part of 'flutter_godot.dart';

final class FlutterGodotCompat {
  const FlutterGodotCompat._();

  /// Register the flutter_godot plugin.
  /// Plugin registration is managed by the Flutter framework, please do not register manually.
  /// Provides compatibility handling for unsupported platforms and platform-specific registration.
  static void registerWith() {
    if (Platform.isLinux) {
      FlutterGodotPlatform.instance = FlutterGodotLinux();
    } else {
      FlutterGodotPlatform.instance = FlutterGodotUnsupported();
    }
  }
}
