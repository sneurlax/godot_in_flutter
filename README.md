# flutter_godot

The `flutter_godot` plugin allows you to embed Godot games as widgets into Flutter applications with two-way communication support.

![Pub Version](https://img.shields.io/pub/v/flutter_godot?style=flat-square&logo=dart&logoColor=white&label=Pub%20Version&color=blue)
![GitHub Repo stars](https://img.shields.io/github/stars/wyq0918dev/flutter_godot?style=flat-square&logo=github&logoColor=white&label=GitHub%20Stars&color=blue)
![GitHub License](https://img.shields.io/github/license/wyq0918dev/flutter_godot?style=flat-square&logo=github&logoColor=white&label=GitHub%20License)

<img src="https://raw.githubusercontent.com/wyq0918dev/flutter_godot/master/screenshot.png" width="256">

## Version Compatibility

| flutter_godot Version | Flutter Version | Godot Engine Version | Platforms |
|---|---|---|---|
| 0.0.4+webview | 3.35 | 4.5.x | Linux (tested), Android (tested) |

## Platform Support

| Platform | Status | Rendering | IPC |
|---|---|---|---|
| Linux | Tested, working | CEF WebView (Godot WASM) | WebSocket |
| Android | Tested, working | Native PlatformView | Native |
| macOS | Compatibility stub | — | — |
| Windows | Compatibility stub | — | — |
| iOS | Compatibility stub | — | — |
| Web | Compatibility stub | — | — |

## How It Works

On Linux, Godot is exported as WebAssembly and runs inside a CEF (Chromium Embedded Framework) WebView embedded inline in the Flutter widget tree. A localhost HTTP server serves the WASM files, and a WebSocket server handles bidirectional communication.

```
Flutter App
 +-- GodotHttpServer (serves WASM files on localhost)
 +-- WebSocketIPC (bidirectional JSON messaging)
 +-- CEF WebView
      +-- index.html + Godot WASM engine
      +-- JS WebSocket bridge (relays messages)
```

**Godot -> Flutter:** GDScript `print("__GODOT_IPC__:" + json)` -> JS `console.log` interception -> WebSocket -> Flutter `dataStream`

**Flutter -> Godot:** Flutter `sendDataToGodot()` -> WebSocket -> JS bridge -> `window._flutterMessages[]` -> GDScript `JavaScriptBridge.eval()` poll

On Android, Godot runs natively via PlatformView with direct input handling.

## Setup

### Godot Binary

The Godot binary (`example/assets/bin/godot`) is **not in version control** (~130 MB). To set it up:

```bash
cd example
./DOWNLOAD_GODOT.sh
```

Or download manually from https://godotengine.org/download/linux.

### Exporting the Godot Project

After modifying `example/godot_project/main.gd`, re-export for both targets:

**Linux/WebView (WASM):**
```bash
cd example/godot_project
../assets/bin/godot --headless --export "Web" ../assets/godot_web/index.html
```

Note: The WebSocket bridge JavaScript in `index.html` must be re-injected after each export, as the export overwrites the file.

**Android/native (.pck):**
```bash
cd example/godot_project
../assets/bin/godot --export-pack "Linux/X11" game.pck --headless
cp game.pck ../assets/godot_game.pck
```

## Usage

1. Add the dependency:

    ```yaml
    dependencies:
      flutter_godot: ^latest
    ```

2. Get dependencies:

    ```shell
    flutter pub get
    ```

3. Import the package:

    ```dart
    import 'package:flutter_godot/flutter_godot.dart';
    ```

4. Embed the Godot player widget:

    ```dart
    FlutterGodot.ofPlayer(name: 'assets/godot_game.pck')
    ```

5. Send data to Godot:

    ```dart
    FlutterGodot.sendDataToGodot(data: 'Hello from Flutter');
    ```

6. Listen for data from Godot:

    ```dart
    FlutterGodot.listenGodotData(callback: (String data) {
      print('Received: $data');
    });
    ```

## Architecture Details

### Key Files

| File | Purpose |
|---|---|
| `lib/src/linux.dart` | Linux platform: orchestrates HTTP server, WebSocket IPC, and CEF WebView |
| `lib/src/godot_player.dart` | Widget that creates and displays the CEF WebView (Linux) or PlatformView (Android) |
| `lib/src/websocket_ipc.dart` | WebSocket server for bidirectional JSON messaging |
| `lib/src/godot_http_server.dart` | HTTP server that extracts and serves WASM files from Flutter assets |
| `linux/flutter_godot.cc` | Minimal native plugin registration stub |
| `example/assets/godot_web/index.html` | Godot WASM export with injected WebSocket bridge |
| `example/godot_project/main.gd` | GDScript: console IPC for web mode, file IPC fallback for native |

### WebSocket IPC Protocol

Messages are JSON objects:

```json
{"type": "data", "payload": "string content", "timestamp": 1234567890}
```

Input events:

```json
{"type": "input_event", "x": 100.0, "y": 200.0, "event_type": "down", "button": 0, "pressure": 1.0}
```

GDScript sends messages by printing with a prefix:
```gdscript
print("__GODOT_IPC__:" + JSON.stringify(message))
```

### Build Notes

**webview_cef integration:** The webview_cef plugin modifies the app's `main.cc` and `my_application.cc` to add CEF subprocess initialization (`initCEFProcesses`) and key event handling. The runner files at `example/linux/runner/` must contain these patches. Symlinks in `example/linux/` point to the runner files for webview_cef's auto-patching.

**Cross-Origin headers:** Godot WASM with threads requires `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`. The `GodotHttpServer` sets these on all responses.

**FLUTTER_PLUGIN_EXPORT macro:** Both `flutter_godot` and `webview_cef` define this macro. Our headers use `#ifndef` guards to avoid redefinition errors.

### Fallback Mechanisms

1. **WebSocket -> JS injection:** If no WebSocket clients are connected, `sendDataToGodot()` falls back to `evaluateJavascript()`.
2. **Fallback HTML:** `fallback.html` provides a standalone JS app for testing WebSocket IPC without Godot WASM.
3. **Native file IPC:** `main.gd` detects `OS.has_feature("web")` and falls back to file-based IPC for native mode.

## Known Issues

- HotRestart causes display issues. Use HotReload or recompile.
- The CEF dependency adds ~1.4GB (`libcef.so`) to the build output.
- WebSocket server uses a dynamic port (OS-assigned) to avoid port conflicts between runs.
