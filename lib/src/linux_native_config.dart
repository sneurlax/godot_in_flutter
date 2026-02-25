import 'dart:io';

/// Configuration for the native LibGodot embedding mode on Linux.
///
/// Provides paths and rendering settings for loading libgodot.so
/// and running the Godot game engine natively within the Flutter process.
final class LinuxNativeConfig {
  /// Path to the libgodot.so shared library.
  /// If null, default search paths are used.
  final String? libgodotPath;

  /// Path to the Godot .pck game package file.
  /// If null, looks for godot_game.pck in the assets directory.
  final String? pckPath;

  /// Godot rendering driver: 'vulkan', 'opengl3', or 'dummy'.
  /// Defaults to 'vulkan'.
  final String renderingDriver;

  /// Godot display driver: 'embedded' (headless, renders to texture).
  /// Must be 'embedded' for Flutter integration.
  final String displayDriver;

  /// Initial render width in pixels.
  final int width;

  /// Initial render height in pixels.
  final int height;

  const LinuxNativeConfig({
    this.libgodotPath,
    this.pckPath,
    this.renderingDriver = 'vulkan',
    this.displayDriver = 'embedded',
    this.width = 1280,
    this.height = 720,
  });

  /// Default search paths for libgodot.so, in priority order.
  static List<String> get defaultLibgodotPaths {
    final exeDir = File(Platform.resolvedExecutable).parent.path;
    return [
      '$exeDir/lib/libgodot.so',
      '$exeDir/libgodot.so',
      '/usr/local/lib/libgodot.so',
      '/usr/lib/libgodot.so',
    ];
  }

  /// Default search paths for the .pck game file, in priority order.
  static List<String> get defaultPckPaths {
    final exeDir = File(Platform.resolvedExecutable).parent.path;
    return [
      '$exeDir/data/flutter_assets/assets/godot_game.pck',
      '$exeDir/assets/godot_game.pck',
      '$exeDir/godot_game.pck',
    ];
  }

  /// Find libgodot.so by checking configured path then defaults.
  /// Returns null if not found.
  static String? findLibgodot({String? configuredPath}) {
    if (configuredPath != null && File(configuredPath).existsSync()) {
      return configuredPath;
    }
    for (final path in defaultLibgodotPaths) {
      if (File(path).existsSync()) {
        return path;
      }
    }
    return null;
  }

  /// Find the .pck game file by checking configured path then defaults.
  /// Returns null if not found.
  static String? findPckFile({String? configuredPath}) {
    if (configuredPath != null && File(configuredPath).existsSync()) {
      return configuredPath;
    }
    for (final path in defaultPckPaths) {
      if (File(path).existsSync()) {
        return path;
      }
    }
    return null;
  }

  /// Check whether libgodot.so is available on this system.
  static bool get isAvailable => findLibgodot() != null;
}
