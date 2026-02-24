import 'dart:async';

import 'package:flutter/widgets.dart';
import 'package:webview_cef/webview_cef.dart';

import 'godot_player.dart';
import 'platform_interface.dart';
import 'listen_callback.dart';
import 'websocket_ipc.dart';
import 'godot_http_server.dart';

/// Linux platform implementation using WebView + WebSocket IPC.
///
/// Architecture:
/// 1. Flutter starts an HTTP server to serve Godot WASM export files
/// 2. Flutter starts a WebSocket server for bidirectional IPC
/// 3. A webview_cef WebView loads the Godot game from the HTTP server
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

  /// The HTTP server serving Godot WASM files
  final GodotHttpServer _httpServer = GodotHttpServer();

  /// WebSocket IPC server for robust bidirectional communication
  WebSocketIPC? _wsIPC;

  /// WebSocket server port (0 = OS-assigned)

  /// The webview_cef controller (set during widget initialization)
  WebViewController? _webviewController;

  /// Completer that resolves when the WebView controller is ready
  final Completer<WebViewController> _controllerCompleter =
      Completer<WebViewController>();

  /// Completer that resolves when _initialize() finishes
  Completer<void>? _initCompleter;

  /// Whether initialization has started
  bool _isInitializing = false;
  bool _isInitialized = false;

  /// Stream subscription for WebSocket IPC data
  StreamSubscription<String>? _ipcSubscription;

  /// Asset directory for Godot web export files
  static const String _webAssetDir = 'assets/godot_web';

  /// Initialize the WebView-based Linux integration
  Future<void> _initialize() async {
    if (_isInitialized) return;
    if (_isInitializing) {
      // Wait for the in-progress initialization to finish
      await _initCompleter?.future;
      return;
    }

    _isInitializing = true;
    _initCompleter = Completer<void>();
    try {
      debugPrint(
          '[FlutterGodotLinux] Initializing WebView + WebSocket IPC...');

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

      // Initialize WebviewManager (must be done before creating controllers)
      debugPrint('[FlutterGodotLinux] Initializing WebviewManager...');
      await WebviewManager().initialize();

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

  /// Set the WebView controller (called from the widget when it's created)
  void setWebViewController(WebViewController controller) {
    _webviewController = controller;
    if (!_controllerCompleter.isCompleted) {
      _controllerCompleter.complete(controller);
    }
  }

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

      // Fallback: direct JavaScript injection via WebView controller
      if (_webviewController != null) {
        final escaped = data.replaceAll('\\', '\\\\').replaceAll("'", "\\'");
        final js = '''
          (function() {
            if (!window._flutterMessages) window._flutterMessages = [];
            window._flutterMessages.push({"type":"data","payload":'$escaped'});
            return true;
          })();
        ''';
        await _webviewController!.evaluateJavascript(js);
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
      if (_webviewController != null) {
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
        await _webviewController!.evaluateJavascript(js);
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

      if (_webviewController != null) {
        await _webviewController!.dispose();
        _webviewController = null;
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
