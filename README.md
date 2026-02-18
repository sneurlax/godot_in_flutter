# flutter_godot

The `flutter_godot` plugin allows you to embed Godot games as widgets into Flutter applications with support for two-way communication.

![Pub Version](https://img.shields.io/pub/v/flutter_godot?style=flat-square&logo=dart&logoColor=white&label=Pub%20Version&color=blue)
![GitHub Repo stars](https://img.shields.io/github/stars/wyq0918dev/flutter_godot?style=flat-square&logo=github&logoColor=white&label=GitHub%20Stars&color=blue)
![GitHub License](https://img.shields.io/github/license/wyq0918dev/flutter_godot?style=flat-square&logo=github&logoColor=white&label=GitHub%20License)

<img src="https://raw.githubusercontent.com/wyq0918dev/flutter_godot/master/screenshot.png" width="256">

## Version Compatibility

flutter_godot Plugin Version | Flutter Version | Godot Engine Version
---- | ---- | ----
0.0.1 | 3.32 | 4.4.1
0.0.2 | 3.32 | 4.4.1
0.0.3 | 3.35 | 4.4.1

## Usage

1. Create a new Flutter project or use an existing one.

2. Add the dependency to `pubspec.yaml`

    ```yaml
    dependencies:
      flutter_godot: ^latest
    ```

3. Get the dependencies

    ```shell
    flutter pub get
    ```

4. Import the dependency in your code

    ```dart
    import 'package:flutter_godot/flutter_godot.dart';
    ```

5. The Godot project supports two development modes:

    - **Integrated Mode**: Create an `assets` folder in the Android platform project of your Flutter project (`android\app\src\main\assets`) and create or place your existing Godot project within it.

    - **Standalone Mode**: Manage your Godot project separately, export it as a `.pck` or `.zip` package, place it in the `assets` folder of your Flutter project, and specify the exported package path and filename in your code.

6. Implement Flutter logic in [main.dart](https://github.com/wyq0918dev/flutter_godot/blob/master/example/lib/main.dart) (click to view complete source code)

7. Implement Godot logic in [main.gd](https://github.com/wyq0918dev/flutter_godot/blob/master/example/android/app/src/main/assets/main.gd) (click to view complete source code)

## Known Issues

- Since Godot only provides Android platform libraries officially, this plugin only supports the Android platform. Compatibility handling is provided for other platforms and will not cause the app to crash.
- Due to Flutter platform component limitations, HotRestart will cause Godot not to display. Please use HotReload instead or recompile the app.
- There is a rare crash (Bug) when reopening the app after exiting.
