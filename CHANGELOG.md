## 0.0.4+webview

* Embed Godot on Linux via CEF WebView running Godot as WebAssembly.
* Add WebSocket IPC for bidirectional Flutter-Godot communication.
* Add GodotHttpServer for serving WASM files from Flutter assets.
* Add webview_cef dependency for inline Chromium WebView rendering.
* Add Godot web export preset and WASM build.
* Replace native X11/XComposite texture capture approach.

## 0.0.3+linux

* Add Linux platform support with file-based IPC.
* Requires Godot 4.5.x (file-based IPC works around StreamPeerTCP bug).
* Add GodotProcess for launching Godot as a subprocess.
* Add FileProtocolIPC for bidirectional Flutter-Godot communication.
* Add native C++ plugin registration for Flutter Linux.
* Add Godot project with export presets for Linux/X11.
* Add DOWNLOAD_GODOT.sh helper script.

## 0.0.3

* Support for Godot Project as an independent project.
* optimize code.
* update dependency.

## 0.0.2

* Fixed wasm incompatibility issue

## 0.0.1

* Preliminary realization of functions
