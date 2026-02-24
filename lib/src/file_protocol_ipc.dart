import 'dart:async';
import 'dart:convert';
import 'dart:io' as io;

/// File-based IPC for Flutter-Godot communication
/// Uses atomic file operations in /tmp/flutter_godot_ipc/{godot_pid}/
final class FileProtocolIPC {
  final int _godotPid;

  late String _ipcDir;
  late String _outgoingDir;
  late String _incomingDir;

  final StreamController<String> _dataStream = StreamController<String>.broadcast();
  Timer? _pollTimer;
  bool _isConnected = false;

  Stream<String> get dataStream => _dataStream.stream;
  bool get isConnected => _isConnected;

  FileProtocolIPC(this._godotPid);

  Future<void> initialize() async {
    try {
      debugLog('[FileProtocolIPC] Initializing file protocol IPC');

      _ipcDir = '/tmp/flutter_godot_ipc_${_godotPid}';
      _outgoingDir = '$_ipcDir/outgoing';
      _incomingDir = '$_ipcDir/incoming';

      // Create directory structure
      await io.Directory(_outgoingDir).create(recursive: true);
      await io.Directory(_incomingDir).create(recursive: true);

      debugLog('[FileProtocolIPC] IPC directory: $_ipcDir');

      // Start polling timer
      _pollTimer = Timer.periodic(const Duration(milliseconds: 100), (_) {
        _pollForMessages();
      });

      _isConnected = true;
      debugLog('[FileProtocolIPC] File protocol IPC initialized');
    } catch (error) {
      debugLog('[FileProtocolIPC] Initialization failed: $error');
      rethrow;
    }
  }

  Future<bool> sendData(String data) async {
    if (!_isConnected) return false;

    try {
      final timestamp = DateTime.now().millisecondsSinceEpoch;
      final message = jsonEncode({
        'type': 'data',
        'payload': data,
        'timestamp': timestamp,
      });

      final tempFile = io.File('$_outgoingDir/msg_${timestamp}_temp');
      final finalFile = io.File('$_outgoingDir/msg_$timestamp');

      // Atomic write: write to temp then rename
      await tempFile.writeAsString(message);
      await tempFile.rename(finalFile.path);

      debugLog('[FileProtocolIPC] Sent: $data');
      return true;
    } catch (error) {
      debugLog('[FileProtocolIPC] Send failed: $error');
      return false;
    }
  }

  Future<void> sendInputEvent({
    required double x,
    required double y,
    required String type,
    required int button,
    double pressure = 1.0,
  }) async {
    if (!_isConnected) return;

    try {
      final eventData = jsonEncode({
        'type': 'input_event',
        'x': x,
        'y': y,
        'event_type': type,
        'button': button,
        'pressure': pressure,
      });

      final timestamp = DateTime.now().millisecondsSinceEpoch;
      final tempFile = io.File('$_outgoingDir/event_${timestamp}_temp');
      final finalFile = io.File('$_outgoingDir/event_$timestamp');

      await tempFile.writeAsString(eventData);
      await tempFile.rename(finalFile.path);

      debugLog('[FileProtocolIPC] Sent input event: $type at ($x, $y)');
    } catch (error) {
      debugLog('[FileProtocolIPC] Send input event failed: $error');
    }
  }

  Future<void> _pollForMessages() async {
    try {
      final incomingDir = io.Directory(_incomingDir);
      if (!incomingDir.existsSync()) return;

      final files = incomingDir.listSync();
      for (var file in files) {
        if (file is io.File) {
          try {
            final content = await file.readAsString();
            final json = jsonDecode(content) as Map<String, dynamic>;

            if (json['type'] == 'data' && json['payload'] != null) {
              final payload = json['payload'] as String;
              _dataStream.add(payload);
              debugLog('[FileProtocolIPC] Received: $payload');
            }

            // Delete after processing
            await file.delete();
          } catch (e) {
            debugLog('[FileProtocolIPC] Error processing file: $e');
          }
        }
      }
    } catch (error) {
      // Silent fail on polling
    }
  }

  Future<void> dispose() async {
    try {
      _pollTimer?.cancel();
      await _dataStream.close();
      _isConnected = false;
    } catch (error) {
      debugLog('[FileProtocolIPC] Dispose error: $error');
    }
  }
}

void debugLog(String message) {
  print(message);
}
