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
    return eventStream.receiveBroadcastStream().listen((dynamic event) {
      if (event is Map && event["type"] != null) {
        switch (event["type"]) {
          case "takeString":
            callback(event["data"]);
            break;
          default:
            debugPrint("Unknown event type: ${event["type"]}");
            break;
        }
      } else {
        debugPrint("Unknown event: $event");
      }
    }, onError: (error) => debugPrint(error.toString()));
  }

  /// Game player widget
  @override
  Widget ofPlayer({String? name, String? package}) {
    return GodotPlayer(name: name, package: package);
  }
}
