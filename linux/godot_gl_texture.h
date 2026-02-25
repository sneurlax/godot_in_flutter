// godot_gl_texture.h -- FlTextureGL subclass for Godot embedded OpenGL rendering
//
// This is the zero-copy texture bridge between Godot and Flutter on Linux.
// Godot renders into an FBO whose color texture lives in a shared GDK GL
// context. Flutter's raster thread calls populate() to read that same
// texture by GL name -- zero copy because both contexts share the same
// GL namespace.
//
// Double buffering:
//   - Godot renders into the "back" FBO
//   - After each frame, we swap front/back
//   - Flutter's populate() always returns the "front" color texture
//   - A mutex protects the swap operation
//
// Architecture:
//   Flutter raster thread: populate() -> returns front colorTexture GL name
//                                         ^ (shared GL namespace)
//   Plugin: GDK shared GL context -> FBO with colorTexture
//                                         ^ (Godot renders here)
//   Godot thread: make_current -> render to FBO -> done_current

#ifndef FLUTTER_GODOT_GODOT_GL_TEXTURE_H_
#define FLUTTER_GODOT_GODOT_GL_TEXTURE_H_

#include <flutter_linux/flutter_linux.h>
#include <gdk/gdk.h>

G_BEGIN_DECLS

#define GODOT_TYPE_GL_TEXTURE (godot_gl_texture_get_type())
G_DECLARE_FINAL_TYPE(GodotGLTexture, godot_gl_texture, GODOT, GL_TEXTURE, FlTextureGL)

/// Create a new GodotGLTexture.
/// @param shared_context  GDK GL context shared with Flutter's (created from same GdkWindow)
/// @param width           Texture width in pixels
/// @param height          Texture height in pixels
/// @return New GodotGLTexture instance, or NULL on failure
GodotGLTexture* godot_gl_texture_new(GdkGLContext* shared_context,
                                      uint32_t width,
                                      uint32_t height);

/// Initialize OpenGL resources (FBOs, textures).
/// Must be called after construction, with the shared GL context current.
/// @return TRUE on success, FALSE on failure
gboolean godot_gl_texture_init_gl(GodotGLTexture* self);

/// Get the GL texture name for the back buffer (Godot renders here).
/// Godot's GLManagerExternal::window_create() should use this as its colorTexture.
uint32_t godot_gl_texture_get_back_texture(GodotGLTexture* self);

/// Get the GL FBO name for the back buffer (Godot's render target).
uint32_t godot_gl_texture_get_back_fbo(GodotGLTexture* self);

/// Swap front and back buffers.
/// Call this after each completed Godot frame.
/// Thread-safe: acquires internal mutex.
void godot_gl_texture_swap_buffers(GodotGLTexture* self);

/// Get the shared GDK GL context.
GdkGLContext* godot_gl_texture_get_context(GodotGLTexture* self);

/// Get texture dimensions.
uint32_t godot_gl_texture_get_width(GodotGLTexture* self);
uint32_t godot_gl_texture_get_height(GodotGLTexture* self);

G_END_DECLS

#endif  // FLUTTER_GODOT_GODOT_GL_TEXTURE_H_
