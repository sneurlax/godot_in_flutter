# flutter_godot (English)

README Language: [简体中文](https://github.com/wyq0918dev/flutter_godot/blob/master/README.md) [English](https://github.com/wyq0918dev/flutter_godot/blob/master/README.en.md)

The `flutter_godot` plugin allows you to embed Godot games as Widgets in Flutter applications, supporting two-way communication between Flutter and Godot.

![Pub Version](https://img.shields.io/pub/v/flutter_godot?style=flat-square&logo=dart&logoColor=white&label=Pub%20Version&color=blue)
![GitHub Repo stars](https://img.shields.io/github/stars/wyq0918dev/flutter_godot?style=flat-square&logo=github&logoColor=white&label=GitHub%20Stars&color=blue)
![GitHub License](https://img.shields.io/github/license/wyq0918dev/flutter_godot?style=flat-square&logo=github&logoColor=white&label=GitHub%20License)

<img src="https://raw.githubusercontent.com/wyq0918dev/flutter_godot/master/screenshot.png" width="256">

## Supported Versions

| flutter_godot Version | Flutter Version | Godot Engine Version | Platforms      |
|----------------------|----------------|---------------------|----------------|
| 0.0.3+linux          | 3.35           | 4.5.x               | Linux (tested) |

## Platform Support

| Platform | Status | IPC Mechanism |
|----------|--------|---------------|
| Linux    | Tested, working | File-based IPC |
| macOS    | Compatibility stub | — |
| Windows  | Compatibility stub | — |
| iOS      | Compatibility stub | — |
| Web      | Compatibility stub | — |

## Linux Support

Linux support requires **Godot 4.5 or later** (tested with 4.5.x releases). The plugin launches Godot as a subprocess and communicates via file-based IPC using JSON message files in `/tmp/`.

Godot's `StreamPeerTCP` socket API has a known bug on Linux where `get_available_bytes()` reports data available but all read methods return error code 1 (WOULD_BLOCK). File-based IPC is used as a reliable alternative.

### Godot Binary

The Linux example requires a Godot binary at `example/assets/bin/godot`. This binary is **not checked into version control** due to its size (~130 MB). To set it up:

```bash
cd example
./DOWNLOAD_GODOT.sh
```

Or download manually from https://godotengine.org/download/linux and place the x86_64 binary at `example/assets/bin/godot`.

### Exporting the Godot Project

After modifying `example/godot_project/main.gd`, you must re-export the `.pck` file:

```bash
cd example/godot_project
../assets/bin/godot --export-pack "Linux/X11" game.pck --headless
cp game.pck ../assets/godot_game.pck
```

The export preset "Linux/X11" is configured in `example/godot_project/export_presets.cfg`. The exported `godot_game.pck` is bundled as a Flutter asset and extracted at runtime to `~/.cache/flutter_godot/`.

## Usage

1. Add the dependency in your `pubspec.yaml`:

    ```yaml
    dependencies:
      flutter_godot: ^latest
    ```

2. Update dependencies:

    ```shell
    flutter pub get
    ```

3. Import the package in your code:

    ```dart
    import 'package:flutter_godot/flutter_godot.dart';
    ```

4. Manage the Godot project separately, export the `.pck` package, and place it in the Flutter project's `assets` folder.

5. Implement Flutter logic ([main.dart](https://github.com/wyq0918dev/flutter_godot/blob/master/example/lib/main.dart))

6. Implement Godot logic ([main.gd](https://github.com/wyq0918dev/flutter_godot/blob/master/example/godot_project/main.gd))

## Known Issues

- Due to Flutter platform component limitations, HotRestart may cause Godot to not display. Prefer using HotReload or recompiling the app.
- On Linux, Godot 4.5's `StreamPeerTCP` cannot read socket data due to a framework bug. File-based IPC is used instead.
