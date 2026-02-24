import 'dart:async';
import 'dart:convert';
import 'dart:io' as io;

import 'package:crypto/crypto.dart';
import 'package:flutter/services.dart';

/// Manages the Godot subprocess lifecycle on Linux
final class GodotProcess {
  /// The spawned Godot process, or null if not running
  io.Process? _process;

  /// Cached process ID
  int? _pid;

  /// Whether the process has been explicitly terminated
  bool _terminated = false;

  /// Lock for thread-safe process operations
  final _lock = AsyncLock();

  /// Initializes and starts the Godot process
  ///
  /// Extracts the asset bundle to ~/.cache/flutter_godot/{sha256(assetPath)}/
  /// if not already cached, then spawns Godot with the --main-pack argument.
  ///
  /// Throws [ProcessException] if the Godot binary is not found.
  /// Throws [IOException] if asset extraction fails.
  Future<void> start({required String assetPath}) async {
    return _lock.run(() => _startLocked(assetPath: assetPath));
  }

  Future<void> _startLocked({required String assetPath}) async {
    // Prevent starting if already running
    if (_process != null && !_terminated) {
      throw StateError('Process already running');
    }

    _terminated = false;

    try {
      // Extract asset to cache
      final extractedPath = await _extractAsset(assetPath);

      // Verify Godot binary exists
      final godotPath = _findGodotBinary();
      if (godotPath == null) {
        _logPrint('[GodotProcess] Godot binary search failed. Checked:');
        _logPrint('[GodotProcess]   - Bundled: ./assets/bin/godot, ./lib/godot, /opt/*, /usr/local/*');
        _logPrint('[GodotProcess]   - User cache: ~/.local/share/flutter_godot/godot');
        _logPrint('[GodotProcess]   - System PATH: godot, godot4');
        _logPrint('[GodotProcess]   - Environment: GODOT_PATH variable');
        throw io.ProcessException(
          'godot',
          ['--main-pack', extractedPath],
          'Godot binary not found. Install Godot or set GODOT_PATH=/path/to/godot',
        );
      }

      // Copy plugin files to current directory for Godot to find
      await _copyPluginFilesToWorkingDir();

      // Spawn Godot process
      _logPrint('[GodotProcess] Starting Godot: $godotPath --main-pack $extractedPath');

      _process = await io.Process.start(
        godotPath,
        ['--main-pack', extractedPath],
      );

      _pid = _process!.pid;
      _logPrint('[GodotProcess] Godot process started with PID: $_pid');

      // Set up stdout/stderr logging
      _setupLogging();
    } catch (e) {
      _process = null;
      _pid = null;
      rethrow;
    }
  }

  /// Terminates the Godot subprocess
  ///
  /// Attempts graceful shutdown via SIGTERM first, then forceful
  /// SIGKILL if the process doesn't exit within a timeout.
  Future<void> kill() async {
    return _lock.run(() => _killLocked());
  }

  Future<void> _killLocked() async {
    if (_process == null) {
      _logPrint('[GodotProcess] No process to kill');
      return;
    }

    _terminated = true;

    try {
      _logPrint('[GodotProcess] Terminating Godot process (PID: $_pid)');

      // Send SIGTERM for graceful shutdown
      _process!.kill(io.ProcessSignal.sigterm);
      _logPrint('[GodotProcess] Sent SIGTERM to process');

      // Wait for graceful exit with timeout
      final exitCodeFuture = _process!.exitCode;
      final timeoutFuture = Future.delayed(const Duration(seconds: 5));

      try {
        await Future.any([exitCodeFuture, timeoutFuture]);
        final exitCode = await exitCodeFuture.timeout(
          const Duration(milliseconds: 100),
          onTimeout: () => -1,
        );
        _logPrint('[GodotProcess] Process exited with code: $exitCode');
      } on TimeoutException {
        // Process didn't exit, force kill
        _logPrint('[GodotProcess] Graceful shutdown timeout, sending SIGKILL');
        _process!.kill(io.ProcessSignal.sigkill);
        await _process!.exitCode;
        _logPrint('[GodotProcess] Process killed');
      }
    } catch (e) {
      _logPrint('[GodotProcess] Error killing process: $e');
    } finally {
      _process = null;
      _pid = null;
    }
  }

  /// Returns whether the process is currently running
  bool get isRunning => _process != null && !_terminated;

  /// Returns the process ID, or null if not running
  int? get pid => _pid;

  /// Extracts the asset from the Flutter asset bundle to the cache directory
  ///
  /// Uses SHA256 of the asset path as cache key to avoid collisions.
  /// Only extracts if not already present in cache.
  Future<String> _extractAsset(String assetPath) async {
    try {
      _logPrint('[GodotProcess] Loading asset: $assetPath');

      // Load asset data from bundle
      final assetData = await rootBundle.load(assetPath);
      final bytes = assetData.buffer.asUint8List();

      // Determine cache directory
      final cacheDir = io.Platform.environment['HOME'] ?? io.Directory.systemTemp.path;
      final cacheHash = sha256.convert(assetPath.codeUnits).toString();
      final extractPath = '$cacheDir/.cache/flutter_godot/$cacheHash';

      _logPrint('[GodotProcess] Cache path: $extractPath');

      // Create directory if needed
      final targetDir = io.Directory(extractPath);
      if (!targetDir.existsSync()) {
        targetDir.createSync(recursive: true);
        _logPrint('[GodotProcess] Created cache directory');
      }

      // Write asset to cache
      final cacheFile = io.File('$extractPath/${assetPath.split('/').last}');
      await cacheFile.writeAsBytes(bytes);
      _logPrint('[GodotProcess] Extracted asset to: ${cacheFile.path}');

      return cacheFile.path;
    } catch (e) {
      _logPrint('[GodotProcess] Asset extraction failed: $e');
      rethrow;
    }
  }

  /// Finds the Godot binary in PATH
  String? _findGodotBinary() {
    _logPrint('[GodotProcess] Current working directory: ${io.Directory.current.path}');

    // First, check for bundled Godot in common app directory locations
    final bundledCandidates = [
      // Development paths (when running from example/ directory)
      './assets/bin/godot',
      '../example/assets/bin/godot',
      '../../example/assets/bin/godot',
      // App bundle directory (when running from release build)
      './lib/godot',
      '../lib/godot',
      '../../lib/godot',
      // Absolute paths for installed app
      '/opt/flutter_godot_example/lib/godot',
      '/usr/local/flutter_godot_example/lib/godot',
      // User home directory cache (if user downloaded bundled version)
      '${io.Platform.environment['HOME']}/.local/share/flutter_godot/godot',
    ];

    for (final bundledPath in bundledCandidates) {
      try {
        final file = io.File(bundledPath);
        if (file.existsSync()) {
          _logPrint('[GodotProcess] Found bundled Godot at: $bundledPath');
          return bundledPath;
        }
      } catch (e) {
        // Continue to next candidate
      }
    }

    // Also try to find godot binary from environment variable GODOT_PATH
    final envGodotPath = io.Platform.environment['GODOT_PATH'];
    if (envGodotPath != null) {
      try {
        final file = io.File(envGodotPath);
        if (file.existsSync()) {
          _logPrint('[GodotProcess] Found Godot from GODOT_PATH: $envGodotPath');
          return envGodotPath;
        }
      } catch (e) {
        _logPrint('[GodotProcess] GODOT_PATH set but file not found: $e');
      }
    }

    // Fall back to system PATH
    const pathCandidates = ['godot', 'godot4'];

    for (final candidate in pathCandidates) {
      try {
        final result = io.Process.runSync('which', [candidate]);
        if (result.exitCode == 0) {
          final path = (result.stdout as String).trim();
          if (path.isNotEmpty) {
            _logPrint('[GodotProcess] Found Godot in PATH: $path');
            return path;
          }
        }
      } catch (e) {
        _logPrint('[GodotProcess] Error finding $candidate in PATH: $e');
      }
    }

    return null;
  }

  /// Copy plugin files (.gdextension and .so) to working directory
  Future<void> _copyPluginFilesToWorkingDir() async {
    try {
      final cwd = io.Directory.current.path;
      final binDir = io.Directory('$cwd/bin');

      // Ensure bin directory exists
      if (!binDir.existsSync()) {
        binDir.createSync(recursive: true);
      }

      // Copy .so file
      final assetSo = io.File('assets/bin/flutter_godot.so');
      final targetSo = io.File('$cwd/bin/flutter_godot.so');
      if (assetSo.existsSync()) {
        await assetSo.copy(targetSo.path);
        _logPrint('[GodotProcess] Copied flutter_godot.so to bin directory');
      }

      // Copy .gdextension file
      final assetExt = io.File('assets/flutter_godot.gdextension');
      final targetExt = io.File('$cwd/flutter_godot.gdextension');
      if (assetExt.existsSync()) {
        await assetExt.copy(targetExt.path);
        _logPrint('[GodotProcess] Copied flutter_godot.gdextension to working directory');
      }
    } catch (e) {
      _logPrint('[GodotProcess] Error copying plugin files: $e');
    }
  }

  /// Start the Rust plugin process independently to create IPC socket
  Future<void> _startPluginProcess() async {
    try {
      final cwd = io.Directory.current.path;
      final soPath = '$cwd/bin/flutter_godot.so';

      if (!io.File(soPath).existsSync()) {
        _logPrint('[GodotProcess] Plugin .so not found at $soPath, skipping plugin startup');
        return;
      }

      // Use a dummy executable with LD_PRELOAD to load and initialize the plugin
      // We use /bin/true as the dummy executable since it does nothing but can be run
      final env = Map<String, String>.from(io.Platform.environment);
      env['LD_PRELOAD'] = soPath;

      final process = await io.Process.start(
        '/bin/true',
        [],
        environment: env,
        runInShell: false,
      );

      // Wait for the process to complete (should be immediate)
      await process.exitCode;
      _logPrint('[GodotProcess] Plugin process initialized successfully');
    } catch (e) {
      _logPrint('[GodotProcess] Error starting plugin process: $e');
    }
  }

  /// Sets up logging for stdout and stderr
  void _setupLogging() {
    if (_process == null) return;

    // Log stdout
    _process!.stdout.transform(utf8.decoder).listen(
      (String line) {
        _logPrint('[Godot stdout] $line');
      },
      onError: (error) {
        _logPrint('[GodotProcess] stdout error: $error');
      },
    );

    // Log stderr
    _process!.stderr.transform(utf8.decoder).listen(
      (String line) {
        _logPrint('[Godot stderr] $line');
      },
      onError: (error) {
        _logPrint('[GodotProcess] stderr error: $error');
      },
    );

    // Log process exit
    _process!.exitCode.then((exitCode) {
      _logPrint('[GodotProcess] Process exited with code: $exitCode');
    });
  }
}

/// Helper class for async locking to prevent race conditions
class AsyncLock {
  Future<dynamic>? _current;

  Future<T> run<T>(Future<T> Function() callback) async {
    final completer = Completer<T>();

    Future<T> next;
    if (_current != null) {
      next = _current!.then((_) => callback());
    } else {
      next = callback();
    }

    _current = next;

    next.then((result) {
      if (!completer.isCompleted) {
        completer.complete(result);
      }
    }).catchError((error) {
      if (!completer.isCompleted) {
        completer.completeError(error);
      }
    });

    return completer.future;
  }
}

/// Custom debugging function that logs with the app's logger
void _logPrint(String message) {
  // ignore: avoid_print
  print('[flutter_godot] $message');
}
