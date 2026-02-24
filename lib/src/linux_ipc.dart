import 'dart:async';
import 'dart:convert';
import 'dart:io';

/// Manages IPC communication with Godot via TCP sockets on Linux
/// Approach-3: Godot connects to our TCP server instead of creating Unix socket
final class GodotIPC {
  final int _godotPid;

  GodotIPC(this._godotPid);

  /// Stream of data received from Godot
  final StreamController<String> _dataStream =
      StreamController<String>.broadcast();

  /// TCP socket connection (Godot client connects to us)
  Socket? _socket;

  /// TCP server socket (listening for Godot to connect)
  ServerSocket? _serverSocket;

  /// Buffer for incomplete incoming messages
  String _incomingBuffer = '';

  /// Connection state
  bool _isConnected = false;

  /// Subscription to socket data stream
  StreamSubscription<List<int>>? _socketSubscription;

  /// TCP port for the IPC server
  static const int tcpPort = 64001;

  /// Getter for the data stream
  Stream<String> get dataStream => _dataStream.stream;

  /// Check if currently connected
  bool get isConnected => _isConnected;

  /// Initialize the IPC channel - create TCP server for Godot to connect to
  /// Approach-3: Instead of Unix sockets, use TCP on localhost
  Future<void> initialize() async {
    try {
      debugLog('[GodotIPC] Initializing TCP IPC server on port $tcpPort');

      // Create TCP server
      _serverSocket = await ServerSocket.bind(
        InternetAddress.loopbackIPv4,
        tcpPort,
      );

      debugLog('[GodotIPC] TCP server listening on 127.0.0.1:$tcpPort');

      // Listen for Godot connections
      _serverSocket!.listen(
        (Socket clientSocket) {
          debugLog('[GodotIPC] Godot client connected from ${clientSocket.remoteAddress.address}:${clientSocket.remotePort}');
          _socket = clientSocket;
          _isConnected = true;
          _socketSubscription = _socket!.listen(
            _handleIncomingData,
            onError: _handleSocketError,
            onDone: _handleSocketClosed,
          );
          // Send initial response to confirm connection
          _sendHeartbeat();
        },
        onError: (error) {
          debugLog('[GodotIPC] Server socket error: $error');
        },
      );
    } catch (error) {
      debugLog('[GodotIPC] Failed to initialize TCP IPC server: $error');
      rethrow;
    }
  }

  /// Send a message to Godot
  Future<void> send(String message) async {
    if (!_isConnected || _socket == null) {
      throw StateError('Not connected to Godot IPC socket');
    }

    try {
      // Format: DATA\n{data}\nEND\n (matches what Godot expects in handle_socket_message)
      final formattedMessage = 'DATA\n$message\nEND\n';
      _socket!.write(formattedMessage);
      await _socket!.flush();
      debugLog('[GodotIPC] Sent message to Godot: $message');
    } catch (error) {
      _isConnected = false;
      throw SocketException('Failed to send message: $error');
    }
  }

  /// Send data to Godot (public interface)
  Future<bool> sendData(String data) async {
    try {
      await send(data);
      return true;
    } catch (error) {
      debugLog('[GodotIPC] Error sending data: $error');
      return false;
    }
  }

  /// Send an input event to Godot
  Future<void> sendInputEvent({
    required double x,
    required double y,
    required String type,
    required int button,
    double pressure = 1.0,
  }) async {
    if (!_isConnected || _socket == null) {
      throw StateError('Not connected to Godot IPC socket');
    }

    try {
      // Format input event as JSON
      final inputEvent = {
        'x': x,
        'y': y,
        'type': type, // "down", "up", or "move"
        'button': button, // 0 for left, 1 for right, 2 for middle, -1 for no button
        'pressure': pressure,
      };

      final jsonString = jsonEncode(inputEvent);

      // Format: INPUT\n{json_input_event}\nEND\n
      final formattedMessage = 'INPUT\n$jsonString\nEND\n';
      _socket!.write(formattedMessage);
      await _socket!.flush();
      debugLog('[GodotIPC] Sent input event to Godot: $jsonString');
    } catch (error) {
      _isConnected = false;
      throw SocketException('Failed to send input event: $error');
    }
  }

  /// Disconnect from the socket
  Future<void> disconnect() async {
    try {
      await _socketSubscription?.cancel();
      await _socket?.close();
      _isConnected = false;
      debugLog('[GodotIPC] Disconnected from Godot IPC socket');
    } catch (error) {
      debugLog('[GodotIPC] Error during disconnect: $error');
    }
  }

  /// Clean up IPC resources
  Future<void> dispose() async {
    try {
      await disconnect();
      await _serverSocket?.close();
      await _dataStream.close();
      debugLog('[GodotIPC] Disposed resources');
    } catch (error) {
      debugLog('[GodotIPC] Error during dispose: $error');
    }
  }

  /// Handle incoming data from the socket
  void _handleIncomingData(List<int> data) {
    try {
      final chunk = utf8.decode(data);
      _incomingBuffer += chunk;

      // Process complete messages in the buffer
      _processMessageBuffer();
    } catch (error) {
      debugLog('[GodotIPC] Error decoding incoming data: $error');
    }
  }

  /// Process the incoming message buffer
  void _processMessageBuffer() {
    // Look for complete messages ending with END\n
    while (_incomingBuffer.contains('END\n')) {
      final endIndex = _incomingBuffer.indexOf('END\n');
      final rawMessage = _incomingBuffer.substring(0, endIndex);
      _incomingBuffer = _incomingBuffer.substring(endIndex + 4);

      _handleCompleteMessage(rawMessage);
    }
  }

  /// Handle a complete message received from Godot
  void _handleCompleteMessage(String rawMessage) {
    try {
      // Parse message format: TYPE\n{data}\n
      final lines = rawMessage.split('\n');

      if (lines.isEmpty) {
        return;
      }

      final messageType = lines[0];

      switch (messageType) {
        case 'DATA':
          // Extract data between TYPE and first empty line or end
          final dataContent = lines.skip(1).join('\n').trim();
          if (dataContent.isNotEmpty) {
            _dataStream.add(dataContent);
            debugLog('[GodotIPC] Received data from Godot: $dataContent');
          }
          break;

        case 'PONG':
          debugLog('[GodotIPC] Received PONG heartbeat from Godot');
          break;

        case 'ERROR':
          final errorMsg = lines.skip(1).join('\n').trim();
          debugLog('[GodotIPC] Godot error: $errorMsg');
          _dataStream.addError(errorMsg);
          break;

        default:
          debugLog('[GodotIPC] Unknown message type: $messageType');
      }
    } catch (error) {
      debugLog('[GodotIPC] Error processing message: $error');
    }
  }

  /// Handle socket errors
  void _handleSocketError(dynamic error) {
    debugLog('[GodotIPC] Socket error: $error');
    _isConnected = false;
    _dataStream.addError(error);
  }

  /// Handle socket close
  void _handleSocketClosed() {
    debugLog('[GodotIPC] Socket closed by Godot');
    _isConnected = false;
    _socket = null;
  }

  /// Send a heartbeat ping to Godot
  Future<void> _sendHeartbeat() async {
    try {
      if (_socket != null && _isConnected) {
        _socket!.write('PING\n');
        await _socket!.flush();
        debugLog('[GodotIPC] Sent PING heartbeat to Godot');
      }
    } catch (error) {
      debugLog('[GodotIPC] Error sending heartbeat: $error');
    }
  }
}

/// Debug logging helper
void debugLog(String message) {
  // In production, this would use proper logging
  // For now, we'll use print which is available in dart:io
  // ignore: avoid_print
  print(message);
}
