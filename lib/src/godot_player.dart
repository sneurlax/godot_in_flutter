import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:webview_cef/webview_cef.dart';

import '../flutter_godot.dart';
import 'platform_interface.dart';

// Direct import needed for the Linux-specific WebView + WebSocket API
// which is not exposed through the platform interface.
// ignore: unnecessary_import
import 'linux.dart';

// Import native mode for Linux (via flutter_godot.dart which re-exports it)

final class GodotPlayer extends StatefulWidget {
  const GodotPlayer({super.key, this.name, this.package});

  final String? name;
  final String? package;

  static const String _viewType = 'godot-player';

  @override
  State<GodotPlayer> createState() => _GodotPlayerState();
}

class _GodotPlayerState extends State<GodotPlayer> {
  StreamSubscription<dynamic>? _godotDataSubscription;
  bool _isReady = false;
  bool _isWebViewInitialized = false;

  /// webview_cef controller for Linux WebView rendering
  WebViewController? _webviewController;

  /// Native mode texture ID (for LibGodot + FlTextureGL rendering)
  int _nativeTextureId = -1;
  bool _isNativeMode = false;
  bool _isNativeInitialized = false;

  @override
  void initState() {
    super.initState();
    _setupGodotListener();

    // On Linux, check if we should use native mode or WebView mode
    if (Platform.isLinux) {
      final platform = FlutterGodotPlatform.instance;
      if (platform is FlutterGodotLinuxNative) {
        _isNativeMode = true;
        _initializeNative(platform);
      } else {
        _initializeWebView();
      }
    }
  }

  /// Initialize native LibGodot rendering mode
  Future<void> _initializeNative(FlutterGodotLinuxNative platform) async {
    try {
      debugPrint('[GodotPlayer] Starting native mode initialization...');
      final textureId = await platform.initializeNative();
      if (mounted && textureId >= 0) {
        setState(() {
          _nativeTextureId = textureId;
          _isNativeInitialized = true;
          _isReady = true;
        });
        debugPrint('[GodotPlayer] Native mode initialized, textureId=$textureId');
      } else {
        debugPrint('[GodotPlayer] Native mode failed, textureId=$textureId');
      }
    } catch (e, st) {
      debugPrint('[GodotPlayer] Native mode initialization error: $e');
      debugPrint('[GodotPlayer] Stack trace: $st');
    }
  }

  /// Initialize the CEF WebView for Linux
  Future<void> _initializeWebView() async {
    final platform = FlutterGodotPlatform.instance;
    if (platform is! FlutterGodotLinux) return;

    try {
      debugPrint('[GodotPlayer] Starting WebView initialization...');

      // Wait for the platform to be ready (HTTP + WebSocket servers started)
      await platform.ready;

      // Create the WebView controller
      _webviewController = WebviewManager().createWebView(
        loading: const Text('Loading Godot...'),
      );

      // Set up event listener for console messages (monitoring/debug)
      _webviewController!.setWebviewListener(WebviewEventsListener(
        onTitleChanged: (title) {
          debugPrint('[GodotPlayer] WebView title: $title');
        },
        onUrlChanged: (url) {
          debugPrint('[GodotPlayer] WebView URL: $url');
        },
        onConsoleMessage:
            (int level, String message, String source, int line) {
          if (kDebugMode) {
            debugPrint('[GodotPlayer] Console [$level]: $message');
          }
        },
        onLoadStart: (controller, url) {
          debugPrint('[GodotPlayer] WebView load start: $url');
        },
        onLoadEnd: (controller, url) {
          debugPrint('[GodotPlayer] WebView load end: $url');
          if (mounted) {
            setState(() {
              _isReady = true;
            });
          }
        },
      ));

      // Initialize the WebView with the game URL (includes ?ws_port= parameter)
      final gameUrl = platform.gameUrl;
      debugPrint('[GodotPlayer] Loading game from: $gameUrl');
      await _webviewController!.initialize(gameUrl);

      // Register the controller with the platform (for JS injection fallback)
      platform.setWebViewController(_webviewController!);

      if (mounted) {
        setState(() {
          _isWebViewInitialized = true;
        });
      }

      debugPrint('[GodotPlayer] WebView initialized successfully');
    } catch (e, st) {
      debugPrint('[GodotPlayer] WebView initialization error: $e');
      debugPrint('[GodotPlayer] Stack trace: $st');
    }
  }

  /// Set up listener for Godot data stream (triggers initialization)
  void _setupGodotListener() {
    debugPrint('[GodotPlayer] Setting up Godot listener...');
    _godotDataSubscription = FlutterGodot.listenGodotData(
      callback: (String data) {
        if (mounted) {
          debugPrint('[GodotPlayer] Godot data received: $data');
          if (!_isReady) {
            setState(() => _isReady = true);
          }
        }
      },
    );
  }

  @override
  void dispose() {
    debugPrint('[GodotPlayer] Disposing...');
    _godotDataSubscription?.cancel();
    _webviewController?.dispose();
    // Note: native mode resources are managed by the platform instance,
    // not by individual widget instances.
    super.dispose();
  }

  /// Handle pointer events (touch/mouse) and forward to Godot
  Future<void> _handlePointerEvent(PointerEvent event) async {
    final x = event.localPosition.dx;
    final y = event.localPosition.dy;

    String eventType = 'move';
    int button = -1;

    if (event is PointerDownEvent) {
      eventType = 'down';
      button = _getButtonFromPointerEvent(event);
    } else if (event is PointerUpEvent) {
      eventType = 'up';
      button = _getButtonFromPointerEvent(event);
    } else if (event is PointerMoveEvent) {
      eventType = 'move';
      if (event.pressure > 0) {
        button = 0;
      }
    }

    final pressure = event.pressure > 0 ? event.pressure : 1.0;

    try {
      await FlutterGodot.forwardInputEvent(
        x: x,
        y: y,
        type: eventType,
        button: button,
        pressure: pressure,
      );
    } catch (error) {
      if (kDebugMode) {
        debugPrint('[GodotPlayer] Failed to forward input event: $error');
      }
    }
  }

  /// Map pointer button to integer code
  int _getButtonFromPointerEvent(PointerEvent event) {
    if (event is PointerDownEvent || event is PointerUpEvent) {
      switch (event.kind) {
        case PointerDeviceKind.mouse:
          final buttons = event.buttons;
          if (buttons == kPrimaryButton) return 0;
          if (buttons == kSecondaryButton) return 1;
          if (buttons == kTertiaryButton) return 2;
          break;
        case PointerDeviceKind.touch:
        case PointerDeviceKind.stylus:
        case PointerDeviceKind.invertedStylus:
        case PointerDeviceKind.trackpad:
        case PointerDeviceKind.unknown:
          return 0;
      }
    }
    return -1;
  }

  @override
  Widget build(BuildContext context) {
    // Android: use platform view directly
    if (Platform.isAndroid) {
      return _buildAndroidView();
    }

    // Linux: native mode (LibGodot + FlTextureGL) or WebView mode
    if (Platform.isLinux) {
      if (_isNativeMode) {
        return _buildLinuxNative();
      }
      return _buildLinuxWebView();
    }

    // Unsupported platform
    return _buildUnsupportedWidget();
  }

  /// Build the Linux native rendering widget (Godot -> FlTextureGL -> Texture widget)
  Widget _buildLinuxNative() {
    if (!_isNativeInitialized || _nativeTextureId < 0) {
      return _buildLoadingWidget();
    }

    return LayoutBuilder(
      builder: (context, constraints) {
        return Listener(
          onPointerDown: _handlePointerEvent,
          onPointerUp: _handlePointerEvent,
          onPointerMove: _handlePointerEvent,
          child: SizedBox(
            width: constraints.maxWidth,
            height: constraints.maxHeight,
            child: Texture(
              textureId: _nativeTextureId,
              filterQuality: FilterQuality.medium,
            ),
          ),
        );
      },
    );
  }

  /// Build the Linux WebView widget that displays Godot running as WASM
  Widget _buildLinuxWebView() {
    if (_webviewController == null || !_isWebViewInitialized) {
      return _buildLoadingWidget();
    }

    return ValueListenableBuilder(
      valueListenable: _webviewController!,
      builder: (context, bool isReady, child) {
        if (isReady) {
          return _webviewController!.webviewWidget;
        }
        return _webviewController!.loadingWidget;
      },
    );
  }

  /// Build Android native view
  Widget _buildAndroidView() {
    return Listener(
      onPointerDown: _handlePointerEvent,
      onPointerUp: _handlePointerEvent,
      onPointerMove: _handlePointerEvent,
      child: PlatformViewLink(
        surfaceFactory:
            (BuildContext context, PlatformViewController controller) {
          return AndroidViewSurface(
            controller: controller as AndroidViewController,
            hitTestBehavior: PlatformViewHitTestBehavior.opaque,
            gestureRecognizers:
                const <Factory<OneSequenceGestureRecognizer>>{},
          );
        },
        onCreatePlatformView: (PlatformViewCreationParams params) {
          return PlatformViewsService.initExpensiveAndroidView(
            id: params.id,
            viewType: GodotPlayer._viewType,
            layoutDirection: TextDirection.ltr,
            creationParamsCodec: const StandardMessageCodec(),
            creationParams: widget.name != null
                ? widget.package == null
                    ? {'asset_name': widget.name}
                    : {
                        'asset_name':
                            'packages/${widget.package}/${widget.name}',
                      }
                : null,
            onFocus: () => params.onFocusChanged(true),
          )
            ..addOnPlatformViewCreatedListener(params.onPlatformViewCreated)
            ..create();
        },
        viewType: GodotPlayer._viewType,
      ),
    );
  }

  /// Build loading widget during initialization
  Widget _buildLoadingWidget() {
    return Container(
      color: Colors.grey[900],
      child: Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            const SizedBox(
              width: 50,
              height: 50,
              child: CircularProgressIndicator(
                strokeWidth: 3,
              ),
            ),
            const SizedBox(height: 16),
            Text(
              _isNativeMode ? 'Loading Godot...' : 'Loading Godot WebView...',
              style: Theme.of(context).textTheme.bodyLarge?.copyWith(
                    color: Colors.white,
                  ),
            ),
          ],
        ),
      ),
    );
  }

  /// Build widget for unsupported platforms
  Widget _buildUnsupportedWidget() {
    return Container(
      color: Colors.grey[800],
      child: Center(
        child: Text(
          'GodotPlayer is not supported on this platform',
          style: Theme.of(context).textTheme.bodyLarge?.copyWith(
                color: Colors.white,
              ),
          textAlign: TextAlign.center,
        ),
      ),
    );
  }
}
