# flutter_godot example

Demonstrates the flutter_godot plugin with bidirectional Flutter-Godot communication on Linux.

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

This downloads Godot 4.5.x to `assets/bin/godot`. You can also download manually from https://godotengine.org/download/linux and place the x86_64 binary there.

### 2. Export the Godot Project

A pre-built `assets/godot_game.pck` is included. If you modify `godot_project/main.gd`, re-export:

```bash
cd godot_project
../assets/bin/godot --export-pack "Linux/X11" game.pck --headless
cp game.pck ../assets/godot_game.pck
```

### 3. Run

```bash
flutter run -d linux
```

You should see bidirectional messages logged every 2 seconds:
- Flutter sends data to Godot (via file IPC)
- Godot sends "Hello from Godot: {timestamp}" back to Flutter

## Project Structure

```
example/
  assets/
    bin/godot              # Godot binary (not in git, see DOWNLOAD_GODOT.sh)
    godot_game.pck         # Exported Godot game pack (Flutter asset)
    flutter_godot.gdextension
  godot_project/
    project.godot          # Godot project configuration (targets Godot 4.5)
    main.gd                # GDScript: file-based IPC, polls for messages
    main.tscn              # Main scene with Control + Label
    export_presets.cfg     # Export presets (Linux/X11)
    game.pck               # Local copy of exported pack
  lib/
    main.dart              # Flutter app entry point
  DOWNLOAD_GODOT.sh        # Helper to download Godot binary
```

## How IPC Works

Flutter and Godot communicate through files in `/tmp/flutter_godot_ipc_{pid}/`:

- `outgoing/` - Flutter writes JSON messages here, Godot polls and reads them
- `incoming/` - Godot writes JSON messages here, Flutter polls and reads them

Messages use JSON format: `{"type": "data", "payload": "...", "timestamp": ...}`

Polling interval is 100ms on both sides.
