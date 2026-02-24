import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';

import '../flutter_godot.dart';

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

  @override
  void initState() {
    super.initState();
    _setupGodotListener();
  }

  /// Set up listener for Godot data stream (triggers initialization)
  void _setupGodotListener() {
    debugPrint('[GodotPlayer] Setting up Godot listener...');
    _godotDataSubscription = FlutterGodot.listenGodotData(
      callback: (String data) {
        if (mounted) {
          debugPrint('[GodotPlayer] Godot data received: $data');
          // Mark as ready once we receive first data
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
    super.dispose();
  }

  /// Handle pointer events (touch/mouse) and forward to Godot
  Future<void> _handlePointerEvent(PointerEvent event) async {
    // Extract event information
    final x = event.localPosition.dx;
    final y = event.localPosition.dy;

    // Determine event type and button
    String eventType = 'move';
    int button = -1; // No button by default

    if (event is PointerDownEvent) {
      eventType = 'down';
      button = _getButtonFromPointerEvent(event);
    } else if (event is PointerUpEvent) {
      eventType = 'up';
      button = _getButtonFromPointerEvent(event);
    } else if (event is PointerMoveEvent) {
      eventType = 'move';
      // Try to determine button from pressure (for touch)
      if (event.pressure > 0) {
        button = 0; // Left button for touch
      }
    }

    // Get pressure value
    final pressure = event.pressure > 0 ? event.pressure : 1.0;

    // Forward to Godot
    try {
      await FlutterGodot.forwardInputEvent(
        x: x,
        y: y,
        type: eventType,
        button: button,
        pressure: pressure,
      );
      if (kDebugMode) {
        debugPrint(
          '[GodotPlayer] Input event forwarded: type=$eventType, x=$x, y=$y, '
          'button=$button, pressure=$pressure',
        );
      }
    } catch (error) {
      if (kDebugMode) {
        debugPrint('[GodotPlayer] Failed to forward input event: $error');
      }
    }
  }

  /// Map pointer button to integer code
  int _getButtonFromPointerEvent(PointerEvent event) {
    if (event is PointerDownEvent || event is PointerUpEvent) {
      // Check the kind of pointer to determine button
      switch (event.kind) {
        case PointerDeviceKind.mouse:
          // For mouse events, check the buttons
          final buttons = event.buttons;
          if (buttons == kPrimaryButton) return 0; // Left
          if (buttons == kSecondaryButton) return 1; // Right
          if (buttons == kTertiaryButton) return 2; // Middle
          break;
        case PointerDeviceKind.touch:
        case PointerDeviceKind.stylus:
        case PointerDeviceKind.invertedStylus:
        case PointerDeviceKind.trackpad:
        case PointerDeviceKind.unknown:
          return 0; // Default to left button for touch/stylus
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

    // Linux: show loading or ready state
    if (Platform.isLinux) {
      if (_isReady) {
        return _buildLinuxWidget();
      } else {
        return _buildLoadingWidget();
      }
    }

    // Unsupported platform
    return _buildUnsupportedWidget();
  }

  /// Build Android native view
  Widget _buildAndroidView() {
    // Wrap with Listener to capture input events on Android
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

  /// Build loading widget for Linux during initialization
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
              'Godot loading...',
              style: Theme.of(context).textTheme.bodyLarge?.copyWith(
                    color: Colors.white,
                  ),
            ),
          ],
        ),
      ),
    );
  }

  /// Build Linux widget once Godot is ready
  Widget _buildLinuxWidget() {
    // Wrap with Listener to capture input events on Linux
    return Listener(
      onPointerDown: _handlePointerEvent,
      onPointerUp: _handlePointerEvent,
      onPointerMove: _handlePointerEvent,
      child: Container(
        color: Colors.black,
        child: Center(
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              Container(
                padding: const EdgeInsets.all(24),
                decoration: BoxDecoration(
                  border: Border.all(color: Colors.blue, width: 2),
                  borderRadius: BorderRadius.circular(8),
                ),
                child: Text(
                  'Godot Game [Linux]',
                  style: Theme.of(context).textTheme.headlineSmall?.copyWith(
                        color: Colors.blue,
                        fontFamily: 'Courier',
                      ),
                ),
              ),
              const SizedBox(height: 16),
              Text(
                widget.name != null ? 'Asset: ${widget.name}' : 'Ready',
                style: const TextStyle(
                  color: Colors.grey,
                  fontSize: 12,
                ),
              ),
            ],
          ),
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
