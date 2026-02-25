# Embedding Flutter Inside Godot -- Architecture Analysis

## Overview

This document explores the reverse direction: instead of embedding Godot inside
a Flutter app, embedding a Flutter view inside a Godot application. This could
enable Godot games to use Flutter for complex UI overlays, settings screens,
or in-game web browsers.

## Feasibility Assessment

### Core Challenge

Flutter's Linux embedder (`flutter_linux`) is designed to own the top-level
window. It creates an X11/Wayland window, manages its own GL/Vulkan context,
and expects to be the primary renderer. Embedding it as a child within
another renderer (Godot) requires working against these assumptions.

### Possible Approaches

#### Approach 1: Flutter as a Separate Process (X11 Reparenting)

- Run Flutter in a separate process
- Flutter creates its own X11 window
- Godot creates a SubViewport-like container
- Use XReparentWindow to move Flutter's window into Godot's window hierarchy
- Capture Flutter's output via XComposite for texturing

**Pros:**
- Process isolation; Flutter crash does not crash Godot
- Each has its own GL/Vulkan context, no conflicts

**Cons:**
- X11 only (no Wayland support)
- Compositing overhead
- Input forwarding is complex
- Latency from cross-process communication

#### Approach 2: Flutter Custom Embedder API

Flutter provides a low-level embedder API (`flutter_embedder.h`) that allows
hosting Flutter without a window manager. The key functions:

```c
FlutterEngineRun(...)          // Start Flutter engine
FlutterEngineSendWindowMetrics // Set viewport size
FlutterEngineSendPointerEvent  // Forward input events

// Rendering callbacks:
FlutterOpenGLRendererConfig {
    make_current;       // Make GL context current
    clear_current;      // Release GL context
    present;            // Present rendered frame
    fbo_callback;       // Return FBO to render into
    make_resource_current; // For resource loading context
    gl_proc_resolver;   // Resolve GL function pointers
}
```

With the custom embedder, Godot would:

1. Create an EGL context (or share its own)
2. Provide an FBO for Flutter to render into
3. Use the FBO's texture as a Godot ImageTexture or directly as an RD texture
4. Display via a TextureRect or Sprite2D in the Godot scene tree

**Pros:**
- No separate process
- Direct texture sharing (zero-copy if contexts are shared)
- Works on any display server (X11, Wayland, headless)
- Flutter and Godot can share the same GPU context

**Cons:**
- Requires careful thread management (Flutter engine has its own threads)
- GL context sharing can be tricky with Godot's rendering pipeline
- Need to manage Flutter engine lifecycle within Godot

#### Approach 3: Flutter Pixel Buffer

Similar to Approach 2 but simpler:

1. Run Flutter engine with custom embedder
2. Flutter renders to an FBO
3. Read pixels from FBO (glReadPixels or equivalent)
4. Upload to Godot's ImageTexture each frame
5. Display on a TextureRect/Sprite2D

**Pros:**
- Simplest integration, no shared contexts needed
- Works with any Godot rendering backend

**Cons:**
- CPU copy overhead (GPU -> CPU -> GPU)
- Higher latency

### Recommended Architecture

**Approach 2 (Flutter Custom Embedder API)** is the most viable for production:

```
Godot Application
    |
    +-- GDExtension: FlutterView
    |       |
    |       +-- Flutter Engine (via embedder API)
    |       |       |
    |       |       +-- Dart VM running Flutter app
    |       |       |
    |       |       +-- Rendering to shared FBO
    |       |
    |       +-- EGL shared context (Godot <-> Flutter)
    |       |
    |       +-- Godot Texture2D wrapping the FBO output
    |
    +-- TextureRect or Sprite2D displaying the Flutter content
    |
    +-- Input forwarding: Godot InputEvent -> FlutterPointerEvent
```

### Implementation Sketch

A GDExtension plugin `FlutterView` would:

```cpp
// flutter_view.h
class FlutterView : public Node2D {
    GDCLASS(FlutterView, Node2D);

    FlutterEngine engine = nullptr;
    EGLContext shared_context = EGL_NO_CONTEXT;
    GLuint render_fbo = 0;
    GLuint render_texture = 0;
    Ref<ImageTexture> godot_texture;

    // Flutter embedder callbacks
    static bool make_current(void* userdata);
    static bool clear_current(void* userdata);
    static bool present(void* userdata);
    static uint32_t fbo_callback(void* userdata);

public:
    Error start_flutter(const String& p_bundle_path);
    void stop_flutter();
    void set_size(Vector2i p_size);

    // Override _process to pump Flutter events
    void _process(double delta) override;

    // Override _input to forward events
    void _input(const Ref<InputEvent>& p_event) override;
};
```

The rendering flow each frame:

```
1. Godot calls FlutterView::_process()
2. FlutterView::_process() calls FlutterEngineRunTask() for pending tasks
3. Flutter engine calls fbo_callback() -> returns our FBO
4. Flutter engine calls make_current() -> bind shared EGL context
5. Flutter renders its widget tree into the FBO
6. Flutter engine calls present()
7. FlutterView updates godot_texture from render_texture
8. Godot composites the texture into the scene
```

### Key Technical Considerations

1. **Thread Safety**: Flutter engine runs tasks on multiple threads.
   The GL callbacks (make_current, present) happen on Flutter's raster thread.
   Godot's rendering also has its own thread. These must not conflict.
   Solution: Use separate EGL contexts that share textures.

2. **Input Mapping**: Godot's InputEvent system needs to be mapped to
   Flutter's pointer/keyboard event format:
   - InputEventMouseButton -> FlutterPointerEvent (kDown/kUp)
   - InputEventMouseMotion -> FlutterPointerEvent (kMove)
   - InputEventKey -> FlutterKeyEvent

3. **Lifecycle**: Flutter engine needs to be started before Godot starts
   rendering, and stopped before Godot shuts down. This maps well to
   Godot's _ready() and _exit_tree() callbacks.

4. **Platform Channels**: Flutter apps communicate with native code via
   platform channels. The GDExtension could bridge these to Godot signals
   or method calls, enabling bidirectional Flutter<->GDScript communication.

5. **Performance**: With shared GL contexts, the texture is in GPU memory
   and never touches the CPU. The only overhead is the GL fence/sync needed
   to ensure Flutter has finished rendering before Godot reads the texture.

### Comparison: Godot-in-Flutter vs. Flutter-in-Godot

| Aspect | Godot-in-Flutter | Flutter-in-Godot |
|--------|-----------------|-----------------|
| Primary app | Flutter | Godot |
| UI framework | Flutter widgets | Godot UI nodes |
| Game engine | Godot (embedded) | Godot (native) |
| Rendering | Godot renders to texture, Flutter composites | Flutter renders to texture, Godot composites |
| Input | Flutter captures, forwards to Godot | Godot captures, forwards to Flutter |
| Use case | Game widget in a Flutter app | Flutter UI overlay in a Godot game |
| Complexity | Medium (this project) | Medium-High (GDExtension + embedder API) |
| Maturity | Active development | Theoretical (no known implementations) |

### Conclusion

Embedding Flutter inside Godot is technically feasible using Flutter's custom
embedder API. The approach mirrors what we're doing with Godot-in-Flutter but
in reverse. The main challenge is managing two rendering pipelines (Godot's
and Flutter's) on the same GPU without conflicts.

This would be implemented as a GDExtension plugin, making it modular and
optional. The FlutterView node would behave like any other Godot texture
source, fitting naturally into Godot's scene tree.

However, for the fludot project, the Godot-in-Flutter direction is the
correct focus, since the primary use case is adding game content to a Flutter
application, not adding Flutter UI to a Godot game. The Flutter-in-Godot
direction is documented here for completeness and future reference.
