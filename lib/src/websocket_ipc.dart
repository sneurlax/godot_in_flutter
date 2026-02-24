import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/widgets.dart';

/// WebSocket-based IPC server for Flutter-Godot communication.
///
/// Flutter runs a WebSocket server on localhost. The Godot game (running as
/// WASM inside a WebView) connects to this server via JavaScript/GDScript
/// WebSocket. Bidirectional JSON messages flow over the WebSocket connection.
final class WebSocketIPC {
  int _port;

  HttpServer? _httpServer;
  final List<WebSocket> _clients = [];
  final StreamController<String> _dataStream =
      StreamController<String>.broadcast();
  bool _isRunning = false;

  Stream<String> get dataStream => _dataStream.stream;
  bool get isRunning => _isRunning;
  int get clientCount => _clients.length;
  /// The actual port the server is listening on (assigned after start).
  int get port => _port;

  WebSocketIPC({int port = 0}) : _port = port;

  /// Start the WebSocket server on the specified port.
  Future<void> start() async {
    if (_isRunning) return;

    try {
      _httpServer = await HttpServer.bind(
        InternetAddress.loopbackIPv4,
        _port,
      );
      _port = _httpServer!.port;
      _isRunning = true;

      debugPrint('[WebSocketIPC] Server listening on ws://127.0.0.1:$_port');

      _httpServer!.listen((HttpRequest request) {
        _handleRequest(request);
      }, onError: (error) {
        debugPrint('[WebSocketIPC] Server error: $error');
      });
    } catch (e) {
      debugPrint('[WebSocketIPC] Failed to start server: $e');
      rethrow;
    }
  }

  /// Handle incoming HTTP requests and upgrade to WebSocket.
  Future<void> _handleRequest(HttpRequest request) async {
    if (WebSocketTransformer.isUpgradeRequest(request)) {
      try {
        final socket = await WebSocketTransformer.upgrade(request);
        _onClientConnected(socket);
      } catch (e) {
        debugPrint('[WebSocketIPC] WebSocket upgrade failed: $e');
      }
    } else {
      // Respond with 426 Upgrade Required for non-WebSocket requests
      request.response
        ..statusCode = HttpStatus.upgradeRequired
        ..write('WebSocket connection required')
        ..close();
    }
  }

  /// Handle a new WebSocket client connection.
  void _onClientConnected(WebSocket socket) {
    _clients.add(socket);
    debugPrint(
        '[WebSocketIPC] Client connected (total: ${_clients.length})');

    socket.listen(
      (dynamic data) {
        _onMessage(data);
      },
      onError: (error) {
        debugPrint('[WebSocketIPC] Client error: $error');
        _clients.remove(socket);
      },
      onDone: () {
        _clients.remove(socket);
        debugPrint(
            '[WebSocketIPC] Client disconnected (remaining: ${_clients.length})');
      },
    );
  }

  /// Handle an incoming message from a WebSocket client.
  void _onMessage(dynamic data) {
    if (data is String) {
      debugPrint('[WebSocketIPC] Received: $data');

      try {
        final json = jsonDecode(data) as Map<String, dynamic>;
        if (json['type'] == 'data' && json['payload'] != null) {
          final payload = json['payload'] as String;
          _dataStream.add(payload);
        }
      } catch (e) {
        // Not JSON or unexpected format; pass raw data
        _dataStream.add(data);
      }
    }
  }

  /// Send data to all connected WebSocket clients.
  Future<bool> sendData(String data) async {
    if (!_isRunning || _clients.isEmpty) return false;

    final message = jsonEncode({
      'type': 'data',
      'payload': data,
      'timestamp': DateTime.now().millisecondsSinceEpoch,
    });

    final closedClients = <WebSocket>[];
    for (final client in _clients) {
      try {
        client.add(message);
      } catch (e) {
        debugPrint('[WebSocketIPC] Send error: $e');
        closedClients.add(client);
      }
    }

    // Remove any clients that failed
    for (final client in closedClients) {
      _clients.remove(client);
    }

    debugPrint('[WebSocketIPC] Sent to ${_clients.length} client(s): $data');
    return true;
  }

  /// Send an input event to Godot via WebSocket.
  Future<void> sendInputEvent({
    required double x,
    required double y,
    required String type,
    required int button,
    double pressure = 1.0,
  }) async {
    if (!_isRunning || _clients.isEmpty) return;

    final message = jsonEncode({
      'type': 'input_event',
      'x': x,
      'y': y,
      'event_type': type,
      'button': button,
      'pressure': pressure,
    });

    for (final client in _clients) {
      try {
        client.add(message);
      } catch (e) {
        debugPrint('[WebSocketIPC] Send input event error: $e');
      }
    }
  }

  /// Stop the WebSocket server and disconnect all clients.
  Future<void> stop() async {
    if (!_isRunning) return;

    _isRunning = false;

    // Close all client connections
    for (final client in _clients) {
      try {
        await client.close(WebSocketStatus.goingAway, 'Server shutting down');
      } catch (e) {
        debugPrint('[WebSocketIPC] Error closing client: $e');
      }
    }
    _clients.clear();

    // Close the HTTP server
    try {
      await _httpServer?.close(force: true);
    } catch (e) {
      debugPrint('[WebSocketIPC] Error closing server: $e');
    }
    _httpServer = null;

    await _dataStream.close();
    debugPrint('[WebSocketIPC] Server stopped');
  }
}
