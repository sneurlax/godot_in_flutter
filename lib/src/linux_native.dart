import 'dart:async';
import 'dart:io' show pid;

import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'file_protocol_ipc.dart';
import 'godot_player.dart';
import 'linux_native_config.dart';
import 'listen_callback.dart';
import 'platform_interface.dart';

/// Linux platform implementation using native LibGodot embedding.
///
/// Architecture:
/// 1. Flutter loads libgodot.so via the native C plugin (MethodChannel)
/// 2. Godot renders to a shared GL/Vulkan texture
/// 3. Flutter displays the texture via a Texture widget (textureId)
/// 4. Bidirectional IPC uses FileProtocolIPC (file-based message passing)
final class FlutterGodotLinuxNative extends FlutterGodotPlatform {
  FlutterGodotLinuxNative({LinuxNativeConfig? config})
      : _config = config ?? const LinuxNativeConfig();

  /// Register plugin with Flutter's plugin system.
  static void registerWith() {
    FlutterGodotPlatform.instance = FlutterGodotLinuxNative();
  }

  /// Method channel for communicating with the native C plugin.
  final MethodChannel _methodChannel = const MethodChannel(
    'flutter_godot_method',
  );

  /// Configuration for native mode.
  final LinuxNativeConfig _config;

  /// File-based IPC for data exchange with Godot.
  FileProtocolIPC? _ipc;

  /// Stream subscription for IPC data.
  StreamSubscription<String>? _ipcSubscription;

  /// The Flutter texture ID returned by the native plugin.
  int? _textureId;

  /// The Godot process PID (for IPC directory naming).
  int? _godotPid;

  /// Initialization state.
  bool _isInitializing = false;
  bool _isInitialized = false;
  Completer<void>? _initCompleter;

  /// The Flutter texture ID for rendering Godot output.
  int? get textureId => _textureId;

  /// Whether native Godot has been initialized.
  bool get isInitialized => _isInitialized;

  /// Data stream from Godot via FileProtocolIPC.
  Stream<String>? get dataStream => _ipc?.dataStream;

  /// Initialize the native LibGodot integration.
  ///
  /// Calls 'initializeNative' on the method channel, which:
  /// 1. Loads libgodot.so
  /// 2. Starts Godot with embedded display server
  /// 3. Registers a Flutter texture and returns its ID
  /// 4. Returns the PID for IPC setup
  Future<void> initialize({
    String? libgodotPath,
    String? pckPath,
    int? width,
    int? height,
  }) async {
    if (_isInitialized) return;
    if (_isInitializing) {
      await _initCompleter?.future;
      return;
    }

    _isInitializing = true;
    _initCompleter = Completer<void>();

    try {
      debugPrint('[LinuxNative] Initializing native Godot embedding...');

      // Resolve paths
      final resolvedLibPath = LinuxNativeConfig.findLibgodot(
        configuredPath: libgodotPath ?? _config.libgodotPath,
      );
      final resolvedPckPath = LinuxNativeConfig.findPckFile(
        configuredPath: pckPath ?? _config.pckPath,
      );

      if (resolvedLibPath == null) {
        throw FlutterError(
          'libgodot.so not found. Searched paths:\n'
          '  ${LinuxNativeConfig.defaultLibgodotPaths.join("\n  ")}',
        );
      }

      debugPrint('[LinuxNative] Using libgodot: $resolvedLibPath');
      if (resolvedPckPath != null) {
        debugPrint('[LinuxNative] Using pck: $resolvedPckPath');
      }

      // Call the native plugin to initialize Godot
      final result = await _methodChannel.invokeMapMethod<String, dynamic>(
        'initializeNative',
        {
          'libgodotPath': resolvedLibPath,
          'pckPath': resolvedPckPath,
          'renderingDriver': _config.renderingDriver,
          'displayDriver': _config.displayDriver,
          'width': width ?? _config.width,
          'height': height ?? _config.height,
        },
      );

      if (result == null) {
        throw FlutterError('Native plugin returned null from initializeNative');
      }

      _textureId = result['textureId'] as int?;
      _godotPid = result['pid'] as int?;

      if (_textureId == null) {
        throw FlutterError('Native plugin did not return a textureId');
      }

      debugPrint('[LinuxNative] Texture ID: $_textureId, PID: $_godotPid');

      // Set up FileProtocolIPC for bidirectional data exchange.
      // Use the Godot PID if available, otherwise fall back to our own PID.
      final ipcPid = _godotPid ?? pid;
      _ipc = FileProtocolIPC(ipcPid);
      await _ipc!.initialize();

      debugPrint('[LinuxNative] FileProtocolIPC initialized (pid: $ipcPid)');

      _isInitialized = true;
      _initCompleter?.complete();
      debugPrint('[LinuxNative] Native Godot embedding initialized');
    } catch (error, stackTrace) {
      debugPrint('[LinuxNative] Initialization error: $error');
      debugPrint('[LinuxNative] Stack trace: $stackTrace');
      _isInitializing = false;
      _initCompleter?.completeError(error);
      rethrow;
    }
  }

  /// Notify the native plugin of a viewport resize.
  Future<void> resize({required int width, required int height}) async {
    if (!_isInitialized) return;
    try {
      await _methodChannel.invokeMethod<void>('resizeNative', {
        'width': width,
        'height': height,
      });
    } catch (error) {
      debugPrint('[LinuxNative] Resize error: $error');
    }
  }

  /// Wait for initialization to complete.
  Future<void> get ready => initialize();

  /// Send data to Godot via FileProtocolIPC.
  @override
  Future<bool> sendDataToGodot({required String data}) async {
    if (_ipc == null || !_ipc!.isConnected) {
      debugPrint('[LinuxNative] IPC not connected, cannot send data');
      return false;
    }
    return _ipc!.sendData(data);
  }

  /// Forward input events to Godot via FileProtocolIPC.
  @override
  Future<void> forwardInputEvent({
    required double x,
    required double y,
    required String type,
    required int button,
    double pressure = 1.0,
  }) async {
    if (_ipc == null || !_ipc!.isConnected) return;
    await _ipc!.sendInputEvent(
      x: x,
      y: y,
      type: type,
      button: button,
      pressure: pressure,
    );
  }

  /// Listen for data sent from Godot via FileProtocolIPC.
  @override
  StreamSubscription<dynamic> listenGodotData({
    required GodotListenCallback callback,
  }) {
    debugPrint('[LinuxNative] Setting up Godot data listener...');

    // Start initialization if not already done, then listen on IPC stream.
    initialize().then((_) {
      if (_ipc != null) {
        _ipcSubscription = _ipc!.dataStream.listen(
          (String data) {
            debugPrint('[LinuxNative] Received from Godot: $data');
            callback(data);
          },
          onError: (Object error, StackTrace stackTrace) {
            debugPrint('[LinuxNative] IPC stream error: $error');
          },
        );
      }
    }).catchError((Object error, StackTrace stackTrace) {
      debugPrint('[LinuxNative] Error in listenGodotData: $error');
    });

    // Return a placeholder subscription (matches FlutterGodotLinux pattern).
    return const Stream<dynamic>.empty().listen(null);
  }

  /// Game player widget.
  @override
  Widget ofPlayer({String? name, String? package}) {
    return GodotPlayer(name: name, package: package);
  }

  /// Dispose all resources.
  Future<void> dispose() async {
    try {
      debugPrint('[LinuxNative] Disposing resources...');

      await _ipcSubscription?.cancel();
      _ipcSubscription = null;

      await _ipc?.dispose();
      _ipc = null;

      if (_isInitialized) {
        try {
          await _methodChannel.invokeMethod<void>('stopNative');
        } catch (error) {
          debugPrint('[LinuxNative] Error stopping native Godot: $error');
        }
      }

      _textureId = null;
      _godotPid = null;
      _isInitialized = false;
      _isInitializing = false;

      debugPrint('[LinuxNative] Resources disposed');
    } catch (error, stackTrace) {
      debugPrint('[LinuxNative] Dispose error: $error');
      debugPrint('[LinuxNative] Stack trace: $stackTrace');
    }
  }
}
