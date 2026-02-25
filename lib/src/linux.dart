import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'godot_player.dart';
import 'platform_interface.dart';
import 'listen_callback.dart';
import 'websocket_ipc.dart';
import 'godot_http_server.dart';

/// Linux platform implementation using native webkit2gtk WebView + WebSocket IPC.
///
/// Architecture:
/// 1. Flutter starts an HTTP server to serve Godot WASM export files
/// 2. Flutter starts a WebSocket server for bidirectional IPC
/// 3. A native webkit2gtk WebView (via FlPixelBufferTexture) loads the Godot
///    game from the HTTP server -- this replaces the 1.4GB webview_cef/CEF dependency
/// 4. JavaScript WebSocket bridge (injected in index.html) connects to
///    Flutter's WebSocket server
/// 5. Godot -> Flutter: Godot print() -> JS console.log interception ->
///    WebSocket -> Flutter
/// 6. Flutter -> Godot: Flutter -> WebSocket -> JS bridge ->
///    window._flutterMessages -> Godot JavaScriptBridge.eval() poll
final class FlutterGodotLinux extends FlutterGodotPlatform {
  FlutterGodotLinux();

  /// Register plugin with Flutter (called by Flutter's plugin system)
  static void registerWith() {
    FlutterGodotPlatform.instance = FlutterGodotLinux();
  }

  /// Method channel for communicating with native webkit2gtk plugin
  static const MethodChannel _channel = MethodChannel('flutter_godot_method');

  /// The HTTP server serving Godot WASM files
  final GodotHttpServer _httpServer = GodotHttpServer();

  /// WebSocket IPC server for robust bidirectional communication
  WebSocketIPC? _wsIPC;

  /// Texture ID returned by the native plugin (-1 = not created)
  int _textureId = -1;

  /// Completer that resolves when _initialize() finishes
  Completer<void>? _initCompleter;

  /// Whether initialization has started
  bool _isInitializing = false;
  bool _isInitialized = false;

  /// Stream subscription for WebSocket IPC data
  StreamSubscription<String>? _ipcSubscription;

  /// Asset directory for Godot web export files
  static const String _webAssetDir = 'assets/godot_web';

  /// Initialize the webkit2gtk-based Linux integration
  Future<void> _initialize() async {
    if (_isInitialized) return;
    if (_isInitializing) {
      await _initCompleter?.future;
      return;
    }

    _isInitializing = true;
    _initCompleter = Completer<void>();
    try {
      debugPrint(
          '[FlutterGodotLinux] Initializing native webkit2gtk WebView + WebSocket IPC...');

      // Start the HTTP server to serve WASM files
      debugPrint('[FlutterGodotLinux] Starting HTTP server...');
      await _httpServer.start(webAssetDir: _webAssetDir);
      debugPrint(
          '[FlutterGodotLinux] HTTP server running at ${_httpServer.baseUrl}');

      // Start the WebSocket IPC server
      debugPrint('[FlutterGodotLinux] Starting WebSocket IPC server...');
      _wsIPC = WebSocketIPC();
      await _wsIPC!.start();
      debugPrint(
          '[FlutterGodotLinux] WebSocket IPC server running on port ${_wsIPC!.port}');

      // Create the native webkit2gtk WebView via method channel
      debugPrint('[FlutterGodotLinux] Creating native WebView...');
      final textureId = await _channel.invokeMethod<int>('createWebView', {
        'width': 1280,
        'height': 720,
      });
      _textureId = textureId ?? -1;
      debugPrint(
          '[FlutterGodotLinux] Native WebView created, texture_id=$_textureId');

      // Load the game URL
      final url = gameUrl;
      debugPrint('[FlutterGodotLinux] Loading game URL: $url');
      await _channel.invokeMethod('loadUrl', {'url': url});

      _isInitialized = true;
      _initCompleter?.complete();
      debugPrint('[FlutterGodotLinux] Initialization complete');
    } catch (error, stackTrace) {
      debugPrint('[FlutterGodotLinux] ERROR during initialization: $error');
      debugPrint('[FlutterGodotLinux] Stack trace: $stackTrace');
      _isInitializing = false;
      _initCompleter?.completeError(error);
      rethrow;
    }
  }

  /// Get the texture ID for the Texture widget
  int get textureId => _textureId;

  /// Get the game URL for the WebView, including WebSocket port parameter
  String get gameUrl =>
      '${_httpServer.gameUrl}?ws_port=${_wsIPC?.port ?? 0}';

  /// Get the HTTP server port
  int get httpPort => _httpServer.port;

  /// Get the WebSocket IPC server port
  int get wsPort => _wsIPC?.port ?? 0;

  /// Get the WebSocket IPC server instance (for monitoring)
  WebSocketIPC? get wsIPC => _wsIPC;

  /// Get or wait for initialization to complete
  Future<void> get ready => _initialize();

  /// Evaluate JavaScript in the WebView
  Future<void> evaluateJavaScript(String script) async {
    try {
      await _channel.invokeMethod('evaluateJavaScript', {'script': script});
    } catch (e) {
      debugPrint('[FlutterGodotLinux] JS evaluation error: $e');
    }
  }

  /// Send data to Godot via WebSocket IPC
  @override
  Future<bool> sendDataToGodot({required String data}) async {
    try {
      debugPrint('[FlutterGodotLinux] sendDataToGodot: $data');

      // Primary path: WebSocket IPC
      if (_wsIPC != null && _wsIPC!.clientCount > 0) {
        final result = await _wsIPC!.sendData(data);
        debugPrint(
            '[FlutterGodotLinux] Data sent to Godot via WebSocket IPC');
        return result;
      }

      // Fallback: direct JavaScript injection via native WebView
      if (_textureId >= 0) {
        final escaped = data.replaceAll('\\', '\\\\').replaceAll("'", "\\'");
        final js = '''
          (function() {
            if (!window._flutterMessages) window._flutterMessages = [];
            window._flutterMessages.push({"type":"data","payload":'$escaped'});
            return true;
          })();
        ''';
        await evaluateJavaScript(js);
        debugPrint(
            '[FlutterGodotLinux] Data sent to Godot via JS injection (fallback)');
        return true;
      }

      debugPrint('[FlutterGodotLinux] No IPC channel available');
      return false;
    } catch (error, stackTrace) {
      debugPrint('[FlutterGodotLinux] ERROR in sendDataToGodot: $error');
      debugPrint('[FlutterGodotLinux] Stack trace: $stackTrace');
      return false;
    }
  }

  /// Forward input events from Flutter to Godot via WebSocket IPC
  @override
  Future<void> forwardInputEvent({
    required double x,
    required double y,
    required String type,
    required int button,
    double pressure = 1.0,
  }) async {
    try {
      // Primary path: WebSocket IPC
      if (_wsIPC != null && _wsIPC!.clientCount > 0) {
        await _wsIPC!.sendInputEvent(
          x: x, y: y, type: type, button: button, pressure: pressure,
        );
        return;
      }

      // Fallback: direct JavaScript injection
      if (_textureId >= 0) {
        final js = '''
          (function() {
            if (!window._flutterMessages) window._flutterMessages = [];
            window._flutterMessages.push({
              "type":"input_event","x":$x,"y":$y,
              "event_type":"$type","button":$button,"pressure":$pressure
            });
            return true;
          })();
        ''';
        await evaluateJavaScript(js);
      }
    } catch (error, stackTrace) {
      debugPrint('[FlutterGodotLinux] ERROR in forwardInputEvent: $error');
      debugPrint('[FlutterGodotLinux] Stack trace: $stackTrace');
    }
  }

  /// Listen for data sent from Godot via WebSocket IPC
  @override
  StreamSubscription<dynamic> listenGodotData({
    required GodotListenCallback callback,
  }) {
    debugPrint('[FlutterGodotLinux] listenGodotData: Setting up listener...');

    _initialize().then((_) {
      debugPrint(
          '[FlutterGodotLinux] Initialization complete, setting up WebSocket IPC stream');
      if (_wsIPC != null) {
        _ipcSubscription = _wsIPC!.dataStream.listen(
          (String data) {
            debugPrint(
                '[FlutterGodotLinux] Received from Godot (WS): $data');
            callback(data);
          },
          onError: (error, stackTrace) {
            debugPrint(
                '[FlutterGodotLinux] WebSocket IPC stream error: $error');
          },
        );
      }
    }).catchError((error, stackTrace) {
      debugPrint(
          '[FlutterGodotLinux] ERROR during listenGodotData initialization: $error');
    });

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
      debugPrint('[FlutterGodotLinux] Disposing resources...');

      await _ipcSubscription?.cancel();
      await _wsIPC?.stop();
      _wsIPC = null;

      // Dispose native webview
      if (_textureId >= 0) {
        try {
          await _channel.invokeMethod('disposeWebView');
        } catch (e) {
          debugPrint('[FlutterGodotLinux] Error disposing native webview: $e');
        }
        _textureId = -1;
      }

      await _httpServer.stop();

      _isInitialized = false;
      _isInitializing = false;
      debugPrint('[FlutterGodotLinux] Resources disposed');
    } catch (error, stackTrace) {
      debugPrint('[FlutterGodotLinux] ERROR during dispose: $error');
      debugPrint('[FlutterGodotLinux] Stack trace: $stackTrace');
    }
  }
}
