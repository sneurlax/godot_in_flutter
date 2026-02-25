part of 'flutter_godot.dart';

final class FlutterGodotCompat {
  const FlutterGodotCompat._();

  /// Register the flutter_godot plugin.
  /// Plugin registration is managed by the Flutter framework, please do not register manually.
  /// Provides compatibility handling for unsupported platforms and platform-specific registration.
  ///
  /// On Linux, checks if libgodot.so is available and registers the native
  /// platform implementation. Falls back to WebView mode otherwise.
  static void registerWith() {
    if (Platform.isLinux) {
      if (LinuxNativeConfig.isAvailable) {
        FlutterGodotPlatform.instance = FlutterGodotLinuxNative();
      } else {
        FlutterGodotPlatform.instance = FlutterGodotLinux();
      }
    } else {
      FlutterGodotPlatform.instance = FlutterGodotUnsupported();
    }
  }
}
