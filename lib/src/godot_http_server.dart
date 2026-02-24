import 'dart:async';
import 'dart:io' as io;

import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

/// A lightweight HTTP server that serves Godot WASM export files from
/// the Flutter asset bundle on localhost.
///
/// This is needed because WebView requires HTTP(S) to load WASM files
/// (file:// URLs don't work due to CORS restrictions).
final class GodotHttpServer {
  io.HttpServer? _server;
  int _port = 0;

  /// The port the server is listening on, or 0 if not started.
  int get port => _port;

  /// The base URL for the served content.
  String get baseUrl => 'http://127.0.0.1:$_port';

  /// The full URL to the Godot game's index.html.
  String get gameUrl => '$baseUrl/index.html';

  /// Directory where the extracted web files are stored.
  String? _extractedDir;

  /// Start the HTTP server on an available port.
  /// [webAssetDir] is the Flutter asset directory path containing the
  /// Godot web export files (e.g., 'assets/godot_web').
  Future<void> start({required String webAssetDir}) async {
    if (_server != null) {
      debugPrint('[GodotHttpServer] Server already running on port $_port');
      return;
    }

    // Extract assets to a temp directory on disk
    _extractedDir = await _extractAssets(webAssetDir);

    // Bind to localhost on a random available port
    _server = await io.HttpServer.bind(io.InternetAddress.loopbackIPv4, 0);
    _port = _server!.port;

    debugPrint('[GodotHttpServer] Serving $webAssetDir on port $_port');
    debugPrint('[GodotHttpServer] Game URL: $gameUrl');

    _server!.listen(_handleRequest);
  }

  /// Extract Godot web export assets from the Flutter asset bundle to disk.
  /// Returns the path to the extracted directory.
  Future<String> _extractAssets(String webAssetDir) async {
    final tempDir = await io.Directory.systemTemp.createTemp('godot_web_');
    final extractPath = tempDir.path;

    debugPrint('[GodotHttpServer] Extracting assets to $extractPath');

    // List of known Godot web export files
    final filesToExtract = [
      'index.html',
      'index.js',
      'index.wasm',
      'index.pck',
      'index.png',
      'index.icon.png',
      'index.apple-touch-icon.png',
      'index.audio.worklet.js',
      'index.audio.position.worklet.js',
    ];

    for (final fileName in filesToExtract) {
      try {
        final assetPath = '$webAssetDir/$fileName';
        final data = await rootBundle.load(assetPath);
        final bytes = data.buffer.asUint8List();
        final outFile = io.File('$extractPath/$fileName');
        await outFile.writeAsBytes(bytes);
        debugPrint('[GodotHttpServer] Extracted: $fileName (${bytes.length} bytes)');
      } catch (e) {
        debugPrint('[GodotHttpServer] Skipping $fileName: $e');
      }
    }

    return extractPath;
  }

  /// Handle an incoming HTTP request by serving the corresponding file.
  void _handleRequest(io.HttpRequest request) {
    final path = request.uri.path;
    final fileName = path == '/' ? '/index.html' : path;
    final filePath = '$_extractedDir$fileName';

    final file = io.File(filePath);

    if (!file.existsSync()) {
      debugPrint('[GodotHttpServer] 404: $path');
      request.response
        ..statusCode = io.HttpStatus.notFound
        ..write('Not found: $path')
        ..close();
      return;
    }

    // Set appropriate MIME type and headers
    final contentType = _getMimeType(fileName);
    request.response.headers
      ..set('Content-Type', contentType)
      ..set('Access-Control-Allow-Origin', '*')
      ..set('Cross-Origin-Opener-Policy', 'same-origin')
      ..set('Cross-Origin-Embedder-Policy', 'require-corp');

    file.openRead().pipe(request.response).catchError((e) {
      debugPrint('[GodotHttpServer] Error serving $path: $e');
    });
  }

  /// Get the MIME type for a file based on its extension.
  String _getMimeType(String fileName) {
    if (fileName.endsWith('.html')) return 'text/html; charset=utf-8';
    if (fileName.endsWith('.js')) return 'application/javascript';
    if (fileName.endsWith('.wasm')) return 'application/wasm';
    if (fileName.endsWith('.pck')) return 'application/octet-stream';
    if (fileName.endsWith('.png')) return 'image/png';
    if (fileName.endsWith('.ico')) return 'image/x-icon';
    if (fileName.endsWith('.json')) return 'application/json';
    if (fileName.endsWith('.css')) return 'text/css';
    return 'application/octet-stream';
  }

  /// Stop the HTTP server and clean up.
  Future<void> stop() async {
    if (_server != null) {
      debugPrint('[GodotHttpServer] Stopping server on port $_port');
      await _server!.close(force: true);
      _server = null;
      _port = 0;
    }

    // Clean up extracted files
    if (_extractedDir != null) {
      try {
        final dir = io.Directory(_extractedDir!);
        if (dir.existsSync()) {
          await dir.delete(recursive: true);
          debugPrint('[GodotHttpServer] Cleaned up temp dir: $_extractedDir');
        }
      } catch (e) {
        debugPrint('[GodotHttpServer] Error cleaning up: $e');
      }
      _extractedDir = null;
    }
  }
}
