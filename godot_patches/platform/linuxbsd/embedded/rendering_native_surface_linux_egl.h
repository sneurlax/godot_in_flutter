/**************************************************************************/
/*  rendering_native_surface_linux_egl.h                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#ifndef RENDERING_NATIVE_SURFACE_LINUX_EGL_H
#define RENDERING_NATIVE_SURFACE_LINUX_EGL_H

#include "core/variant/native_ptr.h"
#include "servers/rendering/rendering_native_surface.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

// Forward declaration for DMA-BUF fd-based texture export.
struct GodotEGLFrameInfo {
	int dma_buf_fd;       // DMA-BUF file descriptor for the rendered frame
	uint32_t width;
	uint32_t height;
	uint32_t stride;      // Row pitch in bytes
	uint32_t format;      // DRM fourcc format (e.g. DRM_FORMAT_ARGB8888)
	uint64_t modifier;    // DRM format modifier
};

// Callback type: host calls this to retrieve the latest rendered frame.
typedef void (*GodotFrameAvailableCallback)(const GodotEGLFrameInfo *frame_info, void *user_data);

/**
 * RenderingNativeSurfaceLinuxEGL
 *
 * Provides an offscreen EGL rendering surface for embedding Godot on Linux.
 * Instead of rendering to an X11 window or Wayland surface, Godot renders
 * to an offscreen EGL PbufferSurface backed by a DMA-BUF exportable image.
 *
 * Two modes of operation:
 *
 * 1. DMA-BUF Export Mode (preferred for zero-copy):
 *    - Godot creates its own EGL context on EGL_PLATFORM_GBM_MESA
 *    - Renders to an FBO backed by an EGLImage from a GBM buffer
 *    - Exports the DMA-BUF fd for the host (Flutter) to import as a GL texture
 *    - Enables zero-copy GPU-to-GPU texture sharing
 *
 * 2. Shared GL Context Mode (simpler, requires same GPU):
 *    - Host provides an EGLDisplay and EGLContext to share with
 *    - Godot creates a shared EGL context
 *    - Renders to an FBO with a GL texture
 *    - Host can directly use the GL texture ID
 *
 * 3. Pixel Readback Mode (fallback, always works):
 *    - Godot renders to an offscreen FBO
 *    - Uses glReadPixels to copy frame data to a CPU buffer
 *    - Host copies the pixel buffer into its own texture
 *    - Works everywhere but has GPU->CPU->GPU copy overhead
 */
class RenderingNativeSurfaceLinuxEGL : public RenderingNativeSurface {
	GDCLASS(RenderingNativeSurfaceLinuxEGL, RenderingNativeSurface);

	static void _bind_methods();

public:
	enum Mode {
		MODE_DMABUF_EXPORT = 0,   // DMA-BUF export for zero-copy sharing
		MODE_SHARED_CONTEXT = 1,   // Shared EGL context with host
		MODE_PIXEL_READBACK = 2,   // glReadPixels fallback
	};

private:
	Mode mode = MODE_PIXEL_READBACK;

	// Surface dimensions.
	uint32_t width = 0;
	uint32_t height = 0;

	// EGL state.
	EGLDisplay egl_display = EGL_NO_DISPLAY;
	EGLContext egl_context = EGL_NO_CONTEXT;
	EGLSurface egl_surface = EGL_NO_SURFACE;
	EGLConfig egl_config = nullptr;

	// Shared context mode: host-provided context for sharing.
	EGLDisplay host_egl_display = EGL_NO_DISPLAY;
	EGLContext host_egl_context = EGL_NO_CONTEXT;

	// FBO for offscreen rendering.
	GLuint fbo = 0;
	GLuint color_texture = 0;
	GLuint depth_renderbuffer = 0;

	// Double-buffered textures for concurrent read/write.
	GLuint front_texture = 0;  // Host reads from this
	GLuint back_texture = 0;   // Godot renders to this

	// Pixel readback buffer.
	uint8_t *pixel_buffer = nullptr;
	uint32_t pixel_buffer_size = 0;

	// DMA-BUF state.
	int dmabuf_fd = -1;

	// Callback for frame availability.
	GodotFrameAvailableCallback frame_callback = nullptr;
	void *frame_callback_userdata = nullptr;

	// Mutex for synchronizing texture swap.
	// In a real implementation this would be std::mutex or Godot's Mutex.
	bool swap_pending = false;

	// Internal initialization methods.
	Error _init_egl_offscreen();
	Error _init_egl_shared(EGLDisplay p_host_display, EGLContext p_host_context);
	Error _create_fbo();
	void _destroy_fbo();
	void _swap_buffers();

public:
	// Static factory methods (matching the pattern from other platforms).

	/**
	 * Create in pixel readback mode (simplest, always works).
	 * p_width, p_height: initial rendering resolution
	 */
	static Ref<RenderingNativeSurfaceLinuxEGL> create_offscreen(uint32_t p_width, uint32_t p_height);

	/**
	 * Create in shared EGL context mode.
	 * p_host_display, p_host_context: the host app's EGL display and context
	 * p_width, p_height: initial rendering resolution
	 */
	static Ref<RenderingNativeSurfaceLinuxEGL> create_shared(
			EGLDisplay p_host_display, EGLContext p_host_context,
			uint32_t p_width, uint32_t p_height);

	/**
	 * GDExtension-compatible factory (passes pointers as uint64_t).
	 */
	static Ref<RenderingNativeSurfaceLinuxEGL> create_api(
			uint64_t p_host_display, uint64_t p_host_context,
			uint32_t p_width, uint32_t p_height);

	// Initialization.
	Error initialize();

	// Get the GL texture that contains the latest rendered frame.
	// In shared context mode, this is a texture shared between Godot and host.
	GLuint get_front_texture() const { return front_texture; }

	// Get the rendering FBO that Godot should render into.
	GLuint get_fbo() const { return fbo; }

	// Get the rendered pixel data (pixel readback mode).
	// Returns nullptr if not in pixel readback mode or no frame rendered yet.
	const uint8_t *get_pixel_data() const { return pixel_buffer; }
	uint32_t get_pixel_data_size() const { return pixel_buffer_size; }

	// Get dimensions.
	uint32_t get_width() const { return width; }
	uint32_t get_height() const { return height; }

	// Resize the rendering surface.
	void resize(uint32_t p_width, uint32_t p_height);

	// Make Godot's EGL context current (call before rendering).
	void make_current();

	// Signal that a frame has been rendered. Performs buffer swap / readback.
	void present_frame();

	// Set callback for frame availability notification.
	void set_frame_callback(GodotFrameAvailableCallback p_callback, void *p_userdata);

	// Get the EGL context (for host to use with shared context).
	EGLContext get_egl_context() const { return egl_context; }
	EGLDisplay get_egl_display() const { return egl_display; }

	// Get DMA-BUF fd (DMA-BUF export mode).
	int get_dmabuf_fd() const { return dmabuf_fd; }

	// Get current mode.
	Mode get_mode() const { return mode; }

	// RenderingNativeSurface interface.
	RenderingContextDriver *create_rendering_context() override;

	RenderingNativeSurfaceLinuxEGL();
	~RenderingNativeSurfaceLinuxEGL();
};

VARIANT_ENUM_CAST(RenderingNativeSurfaceLinuxEGL::Mode);

#endif // RENDERING_NATIVE_SURFACE_LINUX_EGL_H
