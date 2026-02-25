/**************************************************************************/
/*  rendering_native_surface_linux_egl.cpp                                */
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

#include "rendering_native_surface_linux_egl.h"

#include "core/error/error_macros.h"
#include "core/os/memory.h"
#include "core/string/print_string.h"

#include <cstring>

// NOTE: For the Vulkan path, this class is not used. The Vulkan path uses
// RenderingNativeSurfaceLinuxVulkan instead. This class provides OpenGL-based
// offscreen rendering suitable for integration with Flutter's GL texture system.

// ============================================================================
// Bind methods for GDExtension exposure
// ============================================================================

void RenderingNativeSurfaceLinuxEGL::_bind_methods() {
	ClassDB::bind_static_method("RenderingNativeSurfaceLinuxEGL",
			D_METHOD("create", "host_display", "host_context", "width", "height"),
			&RenderingNativeSurfaceLinuxEGL::create_api);

	BIND_ENUM_CONSTANT(MODE_DMABUF_EXPORT);
	BIND_ENUM_CONSTANT(MODE_SHARED_CONTEXT);
	BIND_ENUM_CONSTANT(MODE_PIXEL_READBACK);
}

// ============================================================================
// Static factory methods
// ============================================================================

Ref<RenderingNativeSurfaceLinuxEGL> RenderingNativeSurfaceLinuxEGL::create_offscreen(
		uint32_t p_width, uint32_t p_height) {
	Ref<RenderingNativeSurfaceLinuxEGL> result = memnew(RenderingNativeSurfaceLinuxEGL);
	result->mode = MODE_PIXEL_READBACK;
	result->width = p_width;
	result->height = p_height;
	return result;
}

Ref<RenderingNativeSurfaceLinuxEGL> RenderingNativeSurfaceLinuxEGL::create_shared(
		EGLDisplay p_host_display, EGLContext p_host_context,
		uint32_t p_width, uint32_t p_height) {
	Ref<RenderingNativeSurfaceLinuxEGL> result = memnew(RenderingNativeSurfaceLinuxEGL);
	result->mode = MODE_SHARED_CONTEXT;
	result->host_egl_display = p_host_display;
	result->host_egl_context = p_host_context;
	result->width = p_width;
	result->height = p_height;
	return result;
}

Ref<RenderingNativeSurfaceLinuxEGL> RenderingNativeSurfaceLinuxEGL::create_api(
		uint64_t p_host_display, uint64_t p_host_context,
		uint32_t p_width, uint32_t p_height) {
	if (p_host_display != 0 && p_host_context != 0) {
		return create_shared(
				(EGLDisplay)(uintptr_t)p_host_display,
				(EGLContext)(uintptr_t)p_host_context,
				p_width, p_height);
	} else {
		return create_offscreen(p_width, p_height);
	}
}

// ============================================================================
// Initialization
// ============================================================================

Error RenderingNativeSurfaceLinuxEGL::initialize() {
	Error err;

	switch (mode) {
		case MODE_SHARED_CONTEXT:
			err = _init_egl_shared(host_egl_display, host_egl_context);
			break;
		case MODE_DMABUF_EXPORT:
		case MODE_PIXEL_READBACK:
		default:
			err = _init_egl_offscreen();
			break;
	}

	if (err != OK) {
		return err;
	}

	err = _create_fbo();
	return err;
}

Error RenderingNativeSurfaceLinuxEGL::_init_egl_offscreen() {
	// Create a standalone EGL context using EGL_MESA_platform_surfaceless
	// or EGL_EXT_platform_device for headless rendering.
	// Fallback: use the default display with a PbufferSurface.

	// Try to get the default EGL display first. For truly headless operation,
	// we could use EGL_MESA_platform_surfaceless, but PbufferSurface is
	// more widely supported.
	egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (egl_display == EGL_NO_DISPLAY) {
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, "RenderingNativeSurfaceLinuxEGL: Cannot get EGL display.");
	}

	EGLint major, minor;
	if (!eglInitialize(egl_display, &major, &minor)) {
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, "RenderingNativeSurfaceLinuxEGL: Cannot initialize EGL.");
	}

	print_verbose(vformat("RenderingNativeSurfaceLinuxEGL: EGL %d.%d initialized.", major, minor));

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, "RenderingNativeSurfaceLinuxEGL: Cannot bind OpenGL ES API.");
	}

	// Choose an EGL config that supports PbufferSurface.
	EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_NONE
	};

	EGLint num_configs;
	if (!eglChooseConfig(egl_display, config_attribs, &egl_config, 1, &num_configs) || num_configs == 0) {
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, "RenderingNativeSurfaceLinuxEGL: Cannot find suitable EGL config.");
	}

	// Create a PbufferSurface (we won't actually use it for rendering,
	// we use an FBO instead, but EGL needs a surface to make a context current).
	EGLint pbuffer_attribs[] = {
		EGL_WIDTH, (EGLint)width,
		EGL_HEIGHT, (EGLint)height,
		EGL_NONE
	};

	egl_surface = eglCreatePbufferSurface(egl_display, egl_config, pbuffer_attribs);
	if (egl_surface == EGL_NO_SURFACE) {
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, "RenderingNativeSurfaceLinuxEGL: Cannot create PbufferSurface.");
	}

	// Create an OpenGL ES 3.0 context.
	EGLint context_attribs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3,
		EGL_CONTEXT_MINOR_VERSION, 0,
		EGL_NONE
	};

	egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attribs);
	if (egl_context == EGL_NO_CONTEXT) {
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, "RenderingNativeSurfaceLinuxEGL: Cannot create EGL context.");
	}

	print_verbose("RenderingNativeSurfaceLinuxEGL: Offscreen EGL context created successfully.");
	return OK;
}

Error RenderingNativeSurfaceLinuxEGL::_init_egl_shared(EGLDisplay p_host_display, EGLContext p_host_context) {
	// In shared context mode, we use the host's EGL display and create
	// a context that shares GL objects (textures, buffers) with the host.
	egl_display = p_host_display;

	if (egl_display == EGL_NO_DISPLAY) {
		ERR_FAIL_V_MSG(ERR_INVALID_PARAMETER, "RenderingNativeSurfaceLinuxEGL: Invalid host EGL display.");
	}

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, "RenderingNativeSurfaceLinuxEGL: Cannot bind OpenGL ES API.");
	}

	// Choose the same config as the host would use.
	EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_NONE
	};

	EGLint num_configs;
	if (!eglChooseConfig(egl_display, config_attribs, &egl_config, 1, &num_configs) || num_configs == 0) {
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, "RenderingNativeSurfaceLinuxEGL: Cannot find suitable EGL config for shared context.");
	}

	// Create a Pbuffer surface (needed to make context current).
	EGLint pbuffer_attribs[] = {
		EGL_WIDTH, (EGLint)width,
		EGL_HEIGHT, (EGLint)height,
		EGL_NONE
	};

	egl_surface = eglCreatePbufferSurface(egl_display, egl_config, pbuffer_attribs);
	if (egl_surface == EGL_NO_SURFACE) {
		ERR_FAIL_V_MSG(ERR_CANT_CREATE, "RenderingNativeSurfaceLinuxEGL: Cannot create PbufferSurface for shared context.");
	}

	// Create a shared context. The key here is passing p_host_context as the
	// share_context parameter. This means GL objects created in either context
	// are visible in the other.
	EGLint context_attribs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3,
		EGL_CONTEXT_MINOR_VERSION, 0,
		EGL_NONE
	};

	egl_context = eglCreateContext(egl_display, egl_config, p_host_context, context_attribs);
	if (egl_context == EGL_NO_CONTEXT) {
		EGLint err = eglGetError();
		ERR_FAIL_V_MSG(ERR_CANT_CREATE,
				vformat("RenderingNativeSurfaceLinuxEGL: Cannot create shared EGL context. EGL error: 0x%x", err));
	}

	print_verbose("RenderingNativeSurfaceLinuxEGL: Shared EGL context created successfully.");
	return OK;
}

// ============================================================================
// FBO Management
// ============================================================================

Error RenderingNativeSurfaceLinuxEGL::_create_fbo() {
	make_current();

	// Create the double-buffered textures.
	glGenTextures(1, &front_texture);
	glGenTextures(1, &back_texture);

	// Initialize both textures.
	for (GLuint tex : { front_texture, back_texture }) {
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}

	// Also create a separate color_texture for the primary render target.
	// The back_texture will be blit from this after rendering.
	color_texture = back_texture;

	// Create depth renderbuffer.
	glGenRenderbuffers(1, &depth_renderbuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, depth_renderbuffer);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);

	// Create FBO.
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, back_texture, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_renderbuffer);

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		ERR_FAIL_V_MSG(ERR_CANT_CREATE,
				vformat("RenderingNativeSurfaceLinuxEGL: FBO incomplete, status: 0x%x", status));
	}

	// Allocate pixel readback buffer if in readback mode.
	if (mode == MODE_PIXEL_READBACK) {
		pixel_buffer_size = width * height * 4; // RGBA
		pixel_buffer = (uint8_t *)memalloc(pixel_buffer_size);
		if (!pixel_buffer) {
			ERR_FAIL_V_MSG(ERR_OUT_OF_MEMORY, "RenderingNativeSurfaceLinuxEGL: Cannot allocate pixel buffer.");
		}
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	print_verbose(vformat("RenderingNativeSurfaceLinuxEGL: FBO created (%dx%d).", width, height));
	return OK;
}

void RenderingNativeSurfaceLinuxEGL::_destroy_fbo() {
	make_current();

	if (fbo != 0) {
		glDeleteFramebuffers(1, &fbo);
		fbo = 0;
	}

	if (front_texture != 0 && front_texture != back_texture) {
		glDeleteTextures(1, &front_texture);
	}
	front_texture = 0;

	if (back_texture != 0) {
		glDeleteTextures(1, &back_texture);
		back_texture = 0;
	}

	color_texture = 0;

	if (depth_renderbuffer != 0) {
		glDeleteRenderbuffers(1, &depth_renderbuffer);
		depth_renderbuffer = 0;
	}

	if (pixel_buffer) {
		memfree(pixel_buffer);
		pixel_buffer = nullptr;
		pixel_buffer_size = 0;
	}
}

// ============================================================================
// Rendering operations
// ============================================================================

void RenderingNativeSurfaceLinuxEGL::make_current() {
	if (egl_display != EGL_NO_DISPLAY && egl_context != EGL_NO_CONTEXT) {
		eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
	}
}

void RenderingNativeSurfaceLinuxEGL::present_frame() {
	make_current();

	if (mode == MODE_PIXEL_READBACK && pixel_buffer) {
		// Read pixels from the FBO.
		glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
		glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixel_buffer);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	}

	// In shared context mode, the texture is already shared, but we need to
	// swap front and back to enable double-buffering.
	if (mode == MODE_SHARED_CONTEXT) {
		// Swap front and back textures.
		GLuint tmp = front_texture;
		front_texture = back_texture;
		back_texture = tmp;

		// Reattach the new back texture to the FBO for the next frame.
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, back_texture, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	// Flush to ensure all rendering commands are submitted.
	glFlush();

	// Notify the host that a frame is available.
	if (frame_callback) {
		if (mode == MODE_DMABUF_EXPORT) {
			GodotEGLFrameInfo info = {};
			info.dma_buf_fd = dmabuf_fd;
			info.width = width;
			info.height = height;
			info.stride = width * 4;
			info.format = 0x34325241; // DRM_FORMAT_RGBA8888
			info.modifier = 0;       // DRM_FORMAT_MOD_LINEAR
			frame_callback(&info, frame_callback_userdata);
		} else {
			frame_callback(nullptr, frame_callback_userdata);
		}
	}

	swap_pending = true;
}

void RenderingNativeSurfaceLinuxEGL::resize(uint32_t p_width, uint32_t p_height) {
	if (p_width == width && p_height == height) {
		return;
	}

	width = p_width;
	height = p_height;

	// Recreate the FBO with new dimensions.
	_destroy_fbo();
	_create_fbo();

	// Also recreate the PbufferSurface if we own it.
	if (mode == MODE_PIXEL_READBACK || mode == MODE_DMABUF_EXPORT) {
		if (egl_surface != EGL_NO_SURFACE) {
			eglDestroySurface(egl_display, egl_surface);
		}
		EGLint pbuffer_attribs[] = {
			EGL_WIDTH, (EGLint)width,
			EGL_HEIGHT, (EGLint)height,
			EGL_NONE
		};
		egl_surface = eglCreatePbufferSurface(egl_display, egl_config, pbuffer_attribs);
	}
}

void RenderingNativeSurfaceLinuxEGL::set_frame_callback(
		GodotFrameAvailableCallback p_callback, void *p_userdata) {
	frame_callback = p_callback;
	frame_callback_userdata = p_userdata;
}

// ============================================================================
// RenderingNativeSurface interface
// ============================================================================

RenderingContextDriver *RenderingNativeSurfaceLinuxEGL::create_rendering_context() {
	// The EGL surface is for OpenGL rendering, not Vulkan.
	// When used with DisplayServerEmbedded in OpenGL mode, this would need
	// to create an appropriate OpenGL rendering context driver.
	// For now, the embedded display server only supports Vulkan through
	// RenderingContextDriver. OpenGL goes through a different path (GLES3).
	//
	// For the Flutter integration, we bypass the RenderingContextDriver entirely
	// and use the EGL context directly via make_current() + FBO rendering.
	//
	// If Vulkan is needed, use RenderingNativeSurfaceLinuxVulkan instead.
	ERR_FAIL_V_MSG(nullptr,
			"RenderingNativeSurfaceLinuxEGL does not create a RenderingContextDriver. "
			"Use the EGL context directly for GL rendering, or use "
			"RenderingNativeSurfaceLinuxVulkan for Vulkan rendering.");
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

RenderingNativeSurfaceLinuxEGL::RenderingNativeSurfaceLinuxEGL() {
	// Does nothing.
}

RenderingNativeSurfaceLinuxEGL::~RenderingNativeSurfaceLinuxEGL() {
	_destroy_fbo();

	if (egl_context != EGL_NO_CONTEXT && egl_display != EGL_NO_DISPLAY) {
		eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroyContext(egl_display, egl_context);
		egl_context = EGL_NO_CONTEXT;
	}

	if (egl_surface != EGL_NO_SURFACE && egl_display != EGL_NO_DISPLAY) {
		eglDestroySurface(egl_display, egl_surface);
		egl_surface = EGL_NO_SURFACE;
	}

	// Only terminate the display if we created it (not shared mode).
	if (mode != MODE_SHARED_CONTEXT && egl_display != EGL_NO_DISPLAY) {
		eglTerminate(egl_display);
	}
	egl_display = EGL_NO_DISPLAY;

	if (dmabuf_fd >= 0) {
		// close(dmabuf_fd); // Would need <unistd.h>
		dmabuf_fd = -1;
	}
}
