import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';

import '../flutter_godot.dart';
import 'platform_interface.dart';

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
  bool _linuxInitStarted = false;
  Size? _lastSize;
  Offset? _lastPosition;

  @override
  void initState() {
    super.initState();
    if (!Platform.isLinux) {
      _setupGodotListener();
    }
    // Linux: defer initialization until we know the widget size
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

  /// Called after Linux layout is known to create embed window and start Godot
  void _onLinuxLayout(BuildContext context, BoxConstraints constraints) {
    if (!mounted) return;

    final renderBox = context.findRenderObject() as RenderBox?;
    if (renderBox == null || !renderBox.hasSize) return;

    final size = renderBox.size;
    final position = renderBox.localToGlobal(Offset.zero);

    // First time: set pending size and trigger initialization
    if (!_linuxInitStarted) {
      _linuxInitStarted = true;
      _lastSize = size;
      _lastPosition = position;

      final platform = FlutterGodotPlatform.instance;
      if (platform is FlutterGodotLinux) {
        platform.setEmbedSize(size, position);
      }

      _setupGodotListener();
      return;
    }

    // Subsequent: update embed window if size/position changed
    if (_lastSize != size || _lastPosition != position) {
      _lastSize = size;
      _lastPosition = position;

      final platform = FlutterGodotPlatform.instance;
      if (platform is FlutterGodotLinux) {
        platform.updateEmbedWindow(
          position.dx, position.dy, size.width, size.height,
        );
      }
    }
  }

  @override
  void dispose() {
    debugPrint('[GodotPlayer] Disposing...');
    _godotDataSubscription?.cancel();
    if (Platform.isLinux) {
      final platform = FlutterGodotPlatform.instance;
      if (platform is FlutterGodotLinux) {
        platform.destroyEmbedWindow();
      }
    }
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
    if (Platform.isAndroid) {
      return _buildAndroidView();
    }

    if (Platform.isLinux) {
      return _buildLinuxView();
    }

    return _buildUnsupportedWidget();
  }

  /// Build Linux embedded view using X11 reparenting
  Widget _buildLinuxView() {
    return Listener(
      onPointerDown: _handlePointerEvent,
      onPointerUp: _handlePointerEvent,
      onPointerMove: _handlePointerEvent,
      child: LayoutBuilder(
        builder: (context, constraints) {
          WidgetsBinding.instance.addPostFrameCallback((_) {
            _onLinuxLayout(context, constraints);
          });
          // Transparent container - Godot renders via X11 child window
          return Container(color: Colors.black);
        },
      ),
    );
  }

  /// Build Android native view (touch handled natively by Godot)
  Widget _buildAndroidView() {
    return PlatformViewLink(
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
