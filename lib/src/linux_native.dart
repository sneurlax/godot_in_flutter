import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'godot_player.dart';
import 'listen_callback.dart';
import 'platform_interface.dart';

/// Linux platform implementation using native LibGodot + FlTextureGL.
///
/// Architecture:
/// 1. Flutter creates a shared GDK GL context from the Flutter window
/// 2. Double-buffered FBOs are created in this shared context
/// 3. LibGodot renders into the back FBO via DisplayServerEmbedded
/// 4. After each frame: swap buffers + mark Flutter texture updated
/// 5. Flutter's raster thread reads the front texture via populate()
/// 6. Dart displays via Texture(textureId: ...) widget
///
/// Zero-copy rendering: GDK GL contexts from the same window share the
/// GL namespace, so Godot's color texture is directly readable by Flutter.
final class FlutterGodotLinuxNative extends FlutterGodotPlatform {
  FlutterGodotLinuxNative();

  /// Method channel for communication with the native C plugin
  static const MethodChannel _channel = MethodChannel('flutter_godot_method');

  /// Texture ID returned by the native plugin (-1 if not initialized)
  int _textureId = -1;

  /// Whether native mode is initialized
  bool _initialized = false;

  /// Width and height of the Godot render surface
  int _width = 800;
  int _height = 600;

  /// Stream controller for data received from Godot
  final StreamController<String> _dataController =
      StreamController<String>.broadcast();

  // Getters
  int get textureId => _textureId;
  bool get isInitialized => _initialized;
  int get width => _width;
  int get height => _height;

  /// Initialize the native LibGodot embedding.
  ///
  /// This creates the shared GL context, FBOs, loads libgodot.so,
  /// and starts the Godot engine. Returns the texture ID on success.
  Future<int> initializeNative({
    String? libgodotPath,
    String? pckPath,
    int width = 800,
    int height = 600,
  }) async {
    if (_initialized) return _textureId;

    _width = width;
    _height = height;

    try {
      debugPrint(
          '[FlutterGodotLinuxNative] Initializing native mode ${width}x$height...');

      final result = await _channel.invokeMethod<Map>('initializeNative', {
        if (libgodotPath != null) 'libgodotPath': libgodotPath,
        if (pckPath != null) 'pckPath': pckPath,
        'width': width,
        'height': height,
      });

      if (result != null) {
        _textureId = (result['textureId'] as int?) ?? -1;
        final status = result['status'] as String? ?? 'unknown';
        final mode = result['mode'] as String? ?? 'unknown';

        debugPrint(
            '[FlutterGodotLinuxNative] Initialized: textureId=$_textureId, '
            'status=$status, mode=$mode');

        if (_textureId >= 0) {
          _initialized = true;
        }
      }

      return _textureId;
    } on PlatformException catch (e) {
      debugPrint(
          '[FlutterGodotLinuxNative] PlatformException: ${e.code}: ${e.message}');
      return -1;
    } catch (e, st) {
      debugPrint('[FlutterGodotLinuxNative] Error: $e');
      debugPrint('[FlutterGodotLinuxNative] Stack: $st');
      return -1;
    }
  }

  /// Stop the native Godot engine and clean up resources.
  Future<void> stopNative() async {
    if (!_initialized) return;

    try {
      await _channel.invokeMethod('stopNative');
      _initialized = false;
      _textureId = -1;
      debugPrint('[FlutterGodotLinuxNative] Stopped native mode');
    } catch (e) {
      debugPrint('[FlutterGodotLinuxNative] Error stopping: $e');
    }
  }

  /// Get the current engine status.
  Future<Map<String, dynamic>> getStatus() async {
    try {
      final result = await _channel.invokeMethod<Map>('getStatus');
      return Map<String, dynamic>.from(result ?? {});
    } catch (e) {
      return {'error': e.toString()};
    }
  }

  /// Build the Texture widget that displays Godot's rendered frames.
  Widget buildTextureWidget({
    double? width,
    double? height,
    FilterQuality filterQuality = FilterQuality.medium,
  }) {
    if (!_initialized || _textureId < 0) {
      return SizedBox(
        width: width ?? _width.toDouble(),
        height: height ?? _height.toDouble(),
        child: const ColoredBox(
          color: Color(0xFF1A1A2E),
          child: Center(
            child: Text(
              'Godot not initialized',
              style: TextStyle(color: Color(0xFFCCCCCC)),
            ),
          ),
        ),
      );
    }

    return SizedBox(
      width: width ?? _width.toDouble(),
      height: height ?? _height.toDouble(),
      child: Texture(
        textureId: _textureId,
        filterQuality: filterQuality,
      ),
    );
  }

  // =========================================================================
  // PlatformInterface implementation
  // =========================================================================

  @override
  Future<bool> sendDataToGodot({required String data}) async {
    // TODO: Implement data channel to Godot via GDExtension
    debugPrint('[FlutterGodotLinuxNative] sendDataToGodot: $data (not yet wired)');
    return false;
  }

  @override
  StreamSubscription<dynamic> listenGodotData({
    required GodotListenCallback callback,
  }) {
    return _dataController.stream.listen((data) => callback(data));
  }

  @override
  Future<void> forwardInputEvent({
    required double x,
    required double y,
    required String type,
    required int button,
    double pressure = 1.0,
  }) async {
    // TODO: Implement input forwarding to Godot via GDExtension
    // For now, input goes through Godot's own event system
  }

  @override
  Widget ofPlayer({String? name, String? package}) {
    return GodotPlayer(name: name, package: package);
  }

  /// Dispose resources.
  Future<void> dispose() async {
    await stopNative();
    await _dataController.close();
  }
}
