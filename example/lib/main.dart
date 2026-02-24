import 'dart:async';
import 'dart:io' show Platform;

import 'package:flutter/material.dart';
import 'package:flutter_godot/flutter_godot.dart';

void main() => runApp(const MyApp());

class MyApp extends StatefulWidget {
  const MyApp({super.key});

  @override
  State<MyApp> createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  _MyAppState();

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Flutter Godot',
      theme: ThemeData(
        brightness: Brightness.light,
        colorScheme: ColorScheme.fromSeed(
          seedColor: Colors.purple,
          brightness: Brightness.light,
        ),
      ),
      darkTheme: ThemeData(
        brightness: Brightness.dark,
        colorScheme: ColorScheme.fromSeed(
          seedColor: Colors.purple,
          brightness: Brightness.dark,
        ),
      ),
      home: const HomePage(),
    );
  }
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  StreamSubscription<dynamic>? _sub;
  int _msgCount = 0;
  String _lastMsg = '';

  @override
  void initState() {
    super.initState();
    _sub = FlutterGodot.listenGodotData(callback: (String data) {
      setState(() {
        _msgCount++;
        _lastMsg = data;
      });
    });
  }

  @override
  void dispose() {
    _sub?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Flutter Godot Example')),
      body: Column(
        children: [
          Expanded(
            child: Card.outlined(
              margin: const EdgeInsets.all(16),
              child: ClipRRect(
                borderRadius: const BorderRadius.all(Radius.circular(12.0)),
                child: Platform.isAndroid
                    ? FlutterGodot.ofPlayer()
                    : FlutterGodot.ofPlayer(name: 'assets/godot_game.pck'),
              ),
            ),
          ),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16),
            child: Card.filled(
              child: ListTile(
                leading: const Icon(Icons.message),
                title: Text(_msgCount == 0
                    ? 'No messages from Godot yet'
                    : _lastMsg),
                subtitle: Text('$_msgCount message${_msgCount == 1 ? '' : 's'} received'),
              ),
            ),
          ),
          const SizedBox(height: 80),
        ],
      ),
      floatingActionButtonLocation: FloatingActionButtonLocation.centerFloat,
      floatingActionButton: FloatingActionButton.extended(
        onPressed: () {
          FlutterGodot.sendDataToGodot(data: 'This is data sent from Flutter to Godot.');
        },
        label: const Text('Send message to Godot'),
        icon: const Icon(Icons.send),
      ),
    );
  }
}
