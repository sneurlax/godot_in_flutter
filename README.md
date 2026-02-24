# flutter_godot

README Language: [简体中文](https://github.com/wyq0918dev/flutter_godot/blob/master/README.md) [English](https://github.com/wyq0918dev/flutter_godot/blob/master/README.en.md)

The `flutter_godot` plugin allows you to embed Godot games as widgets into Flutter applications with support for two-way communication.

![Pub Version](https://img.shields.io/pub/v/flutter_godot?style=flat-square&logo=dart&logoColor=white&label=Pub%20Version&color=blue)
![GitHub Repo stars](https://img.shields.io/github/stars/wyq0918dev/flutter_godot?style=flat-square&logo=github&logoColor=white&label=GitHub%20Stars&color=blue)
![GitHub License](https://img.shields.io/github/license/wyq0918dev/flutter_godot?style=flat-square&logo=github&logoColor=white&label=GitHub%20License)

<img src="https://raw.githubusercontent.com/wyq0918dev/flutter_godot/master/screenshot.png" width="256">

## Version Compatibility

flutter_godot Plugin Version | Flutter Version | Godot Engine Version | Platforms
---- | ---- | ---- | ----
0.0.3+linux | 3.35 | 4.5.x | Linux (tested)

## Platform Support

Platform | Status | IPC
---- | ---- | ----
Linux | Tested, working | File-based IPC
macOS | Compatibility stub | —
Windows | Compatibility stub | —
iOS | Compatibility stub | —
Web | Compatibility stub | —

## Linux Support

Linux requires **Godot 4.5 or later** (tested with 4.5.x). The plugin launches Godot as a subprocess and communicates via file-based IPC.

### Godot Binary

The Godot binary (`example/assets/bin/godot`) is **not checked into version control** (~130 MB). To set it up:

```bash
cd example
./DOWNLOAD_GODOT.sh
```

Or download manually from https://godotengine.org/download/linux.

### Exporting the Godot Project

After modifying `example/godot_project/main.gd`, re-export the `.pck`:

```bash
cd example/godot_project
../assets/bin/godot --export-pack "Linux/X11" game.pck --headless
cp game.pck ../assets/godot_game.pck
```

The preset "Linux/X11" is defined in `export_presets.cfg`. The exported `godot_game.pck` is bundled as a Flutter asset.

## Usage

1. Add the dependency to `pubspec.yaml`

    ```yaml
    dependencies:
      flutter_godot: ^latest
    ```

2. Get the dependencies

    ```shell
    flutter pub get
    ```

3. Import the dependency in your code

    ```dart
    import 'package:flutter_godot/flutter_godot.dart';
    ```

4. Manage the Godot project separately, export the `.pck` package, and place it in the Flutter project's `assets` folder.

5. Implement Flutter logic in [main.dart](https://github.com/wyq0918dev/flutter_godot/blob/master/example/lib/main.dart)

6. Implement Godot logic in [main.gd](https://github.com/wyq0918dev/flutter_godot/blob/master/example/godot_project/main.gd)

## Known Issues

- Due to Flutter platform component limitations, HotRestart will cause Godot not to display. Use HotReload or recompile.
- On Linux, Godot 4.5's `StreamPeerTCP` cannot read socket data due to a framework bug. File-based IPC is used instead.
