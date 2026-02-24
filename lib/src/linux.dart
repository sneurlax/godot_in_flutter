import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'godot_player.dart';
import 'platform_interface.dart';
import 'listen_callback.dart';
import 'linux_process.dart';
import 'file_protocol_ipc.dart';

final class FlutterGodotLinux extends FlutterGodotPlatform {
  FlutterGodotLinux();

  /// Register plugin with Flutter (called by Flutter's plugin system)
  static void registerWith() {
    FlutterGodotPlatform.instance = FlutterGodotLinux();
  }

  final MethodChannel methodChannel = const MethodChannel(
    "flutter_godot_method",
  );

  GodotProcess? _godotProcess;
  FileProtocolIPC? _godotIPC;
  StreamSubscription<dynamic>? _ipcSubscription;
  bool _isInitializing = false;
  bool _isInitialized = false;

  /// Initialize the Linux Godot integration
  Future<void> _initialize() async {
    if (_isInitialized) return;
    if (_isInitializing) {
      // Wait for ongoing initialization
      int retries = 50;
      while (_isInitializing && retries > 0) {
        await Future.delayed(const Duration(milliseconds: 100));
        retries--;
      }
      if (_isInitialized) return;
      throw FlutterError('Godot initialization timed out');
    }

    _isInitializing = true;
    try {
      debugPrint('[FlutterGodot] Initializing Linux plugin...');
      _godotProcess = GodotProcess();

      debugPrint('[FlutterGodot] Starting Godot subprocess...');
      await _godotProcess!.start(assetPath: 'assets/godot_game.pck');

      _godotIPC = FileProtocolIPC(_godotProcess!.pid!);

      debugPrint('[FlutterGodot] Initializing file protocol IPC...');
      await _godotIPC!.initialize();

      _isInitialized = true;
      debugPrint('[FlutterGodot] Initialization complete');
    } catch (error, stackTrace) {
      debugPrint('[FlutterGodot] ERROR during initialization: $error');
      debugPrint('[FlutterGodot] Stack trace: $stackTrace');
      _godotProcess = null;
      _godotIPC = null;
      _isInitializing = false;
      rethrow;
    }
    _isInitializing = false;
  }

  /// Send data to Godot via IPC
  @override
  Future<bool> sendDataToGodot({required String data}) async {
    try {
      debugPrint('[FlutterGodot] sendDataToGodot: $data');
      await _initialize();
      if (_godotIPC == null) {
        throw FlutterError('Godot IPC not initialized');
      }
      final result = await _godotIPC!.sendData(data);
      debugPrint('[FlutterGodot] sendDataToGodot result: $result');
      return result;
    } catch (error, stackTrace) {
      debugPrint('[FlutterGodot] ERROR in sendDataToGodot: $error');
      debugPrint('[FlutterGodot] Stack trace: $stackTrace');
      rethrow;
    }
  }

  /// Forward input events from Flutter to Godot
  @override
  Future<void> forwardInputEvent({
    required double x,
    required double y,
    required String type,
    required int button,
    double pressure = 1.0,
  }) async {
    try {
      debugPrint('[FlutterGodot] forwardInputEvent: x=$x, y=$y, type=$type, button=$button, pressure=$pressure');
      await _initialize();
      if (_godotIPC == null) {
        throw FlutterError('Godot IPC not initialized');
      }
      await _godotIPC!.sendInputEvent(
        x: x,
        y: y,
        type: type,
        button: button,
        pressure: pressure,
      );
    } catch (error, stackTrace) {
      debugPrint('[FlutterGodot] ERROR in forwardInputEvent: $error');
      debugPrint('[FlutterGodot] Stack trace: $stackTrace');
      // Don't throw - input forwarding should not crash the app
    }
  }

  /// Listen for data sent from Godot via IPC
  @override
  StreamSubscription<dynamic> listenGodotData({
    required GodotListenCallback callback,
  }) {
    debugPrint('[FlutterGodot] listenGodotData: Setting up listener...');

    // Start initialization asynchronously
    _initialize().then((_) {
      debugPrint('[FlutterGodot] Initialization complete, setting up IPC stream');
      if (_godotIPC != null) {
        _ipcSubscription = _godotIPC!.dataStream.listen(
          (String data) {
            debugPrint('[FlutterGodot] Received from Godot: $data');
            callback(data);
          },
          onError: (error, stackTrace) {
            debugPrint('[FlutterGodot] IPC stream error: $error');
            debugPrint('[FlutterGodot] Stack trace: $stackTrace');
          },
        );
      } else {
        debugPrint('[FlutterGodot] ERROR: _godotIPC is null after initialization');
      }
    }).catchError((error, stackTrace) {
      debugPrint('[FlutterGodot] ERROR during listenGodotData initialization: $error');
      debugPrint('[FlutterGodot] Stack trace: $stackTrace');
    });

    // Return a no-op subscription (data flows through callback, not the stream)
    return const Stream<dynamic>.empty().listen(null);
  }

  /// Game player widget
  @override
  Widget ofPlayer({String? name, String? package}) {
    return GodotPlayer(name: name, package: package);
  }

  /// Dispose resources
  Future<void> dispose() async {
    try {
      debugPrint('[FlutterGodot] Disposing resources...');
      await _ipcSubscription?.cancel();
      if (_godotProcess != null) {
        await _godotProcess!.kill();
      }
      _isInitialized = false;
      _isInitializing = false;
      debugPrint('[FlutterGodot] Resources disposed');
    } catch (error, stackTrace) {
      debugPrint('[FlutterGodot] ERROR during dispose: $error');
      debugPrint('[FlutterGodot] Stack trace: $stackTrace');
    }
  }
}
