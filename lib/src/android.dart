import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'godot_player.dart';
import 'platform_interface.dart';
import 'listen_callback.dart';

final class FlutterGodotAndroid extends FlutterGodotPlatform {
  FlutterGodotAndroid();

  final MethodChannel methodChannel = const MethodChannel(
    "flutter_godot_method",
  );

  final EventChannel eventStream = const EventChannel("flutter_godot_event");

  /// Single broadcast stream shared by all listeners
  late final Stream<String> _godotDataStream = eventStream
      .receiveBroadcastStream()
      .expand<String>((dynamic event) {
        if (event is Map && event["type"] == "takeString") {
          return [event["data"] as String];
        }
        debugPrint("Unknown event: $event");
        return [];
      })
      .asBroadcastStream();

  /// Send data to Godot
  @override
  Future<bool> sendDataToGodot({required String data}) {
    return methodChannel
        .invokeMethod<bool>("sendData2Godot", {"data": data})
        .then((result) => result == true)
        .catchError((error) => throw FlutterError(error.toString()));
  }

  /// Listen for data sent from Godot
  @override
  StreamSubscription<dynamic> listenGodotData({
    required GodotListenCallback callback,
  }) {
    return _godotDataStream.listen(callback);
  }

  /// Game player widget
  @override
  Widget ofPlayer({String? name, String? package}) {
    return GodotPlayer(name: name, package: package);
  }
}
