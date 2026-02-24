# flutter_godot example

Demonstrates the flutter_godot plugin embedding a Godot game inside a Flutter app with bidirectional communication.

## Prerequisites

- Flutter SDK >= 3.35
- Linux x86_64
- Godot 4.5.x binary (see setup below)

## Setup

### 1. Download Godot

The Godot binary is not checked into version control. Run:

```bash
./DOWNLOAD_GODOT.sh
```

This downloads Godot 4.5.x to `assets/bin/godot`. You can also download manually from https://godotengine.org/download/linux.

### 2. Export the Godot Project

Pre-built exports are included. If you modify `godot_project/main.gd`, re-export:

**For Linux/WebView (WASM):**
```bash
cd godot_project
../assets/bin/godot --headless --export "Web" ../assets/godot_web/index.html
```

Note: Re-inject the WebSocket bridge JavaScript into `index.html` after each export.

**For Android/native (.pck):**
```bash
cd godot_project
../assets/bin/godot --export-pack "Linux/X11" game.pck --headless
cp game.pck ../assets/godot_game.pck
```

### 3. Run

```bash
flutter run -d linux
```

## Project Structure

```
example/
  assets/
    bin/godot                  # Godot binary (not in git, see DOWNLOAD_GODOT.sh)
    godot_game.pck             # Exported Godot game pack (Android/native)
    godot_web/                 # Godot WASM export (Linux/WebView)
      index.html               #   HTML with injected WebSocket bridge
      index.js                 #   Godot engine runtime
      index.wasm               #   Godot WebAssembly binary (~35MB)
      index.pck                #   Game resources
      fallback.html            #   Standalone JS fallback for testing
  godot_project/
    project.godot              # Godot project configuration (Godot 4.5)
    main.gd                    # GDScript: console IPC (web) + file IPC (native)
    main.tscn                  # Main scene with Control + Label
    export_presets.cfg         # Export presets (Linux/X11, Android, Web)
  lib/
    main.dart                  # Flutter app entry point
  linux/
    runner/main.cc             # CEF subprocess initialization
    runner/my_application.cc   # CEF key event handling
  DOWNLOAD_GODOT.sh            # Helper to download Godot binary
```

## How It Works

On Linux, the plugin:
1. Starts a localhost HTTP server serving the Godot WASM files
2. Starts a WebSocket server for bidirectional IPC
3. Embeds a CEF WebView loading the Godot game from the HTTP server
4. A JavaScript bridge in `index.html` relays messages between Godot and the WebSocket server

Communication uses JSON messages over WebSocket with a dynamic (OS-assigned) port.
