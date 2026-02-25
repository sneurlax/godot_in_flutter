/**************************************************************************/
/*  rendering_native_surface_linux_vulkan.h                               */
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

#ifndef RENDERING_NATIVE_SURFACE_LINUX_VULKAN_H
#define RENDERING_NATIVE_SURFACE_LINUX_VULKAN_H

#include "core/variant/native_ptr.h"
#include "servers/rendering/rendering_native_surface.h"

#ifdef VULKAN_ENABLED

#ifdef USE_VOLK
#include <volk.h>
#else
#include <vulkan/vulkan.h>
#endif

/**
 * Callback type for frame readback.
 * When a frame is rendered and read back to host memory, this callback is
 * invoked with the pixel data. The host (Flutter) can then upload this to
 * its own texture.
 */
typedef void (*GodotVulkanFrameReadyCallback)(
		const uint8_t *p_pixels,
		uint32_t p_width, uint32_t p_height,
		uint32_t p_stride,
		void *p_user_data);

/**
 * Callback type for DMA-BUF frame export.
 * When a frame is rendered to a DMA-BUF exportable image, this callback is
 * invoked with the file descriptor. The host can import this fd directly
 * as a GL/Vulkan texture for zero-copy compositing.
 */
typedef void (*GodotVulkanDmaBufReadyCallback)(
		int p_dmabuf_fd,
		uint32_t p_width, uint32_t p_height,
		uint32_t p_stride,
		uint32_t p_drm_format,
		uint64_t p_modifier,
		void *p_user_data);

/**
 * RenderingNativeSurfaceLinuxVulkan
 *
 * Provides Vulkan rendering into an offscreen image for embedding Godot
 * on Linux. Instead of rendering to a VkSurfaceKHR backed by a window
 * (X11 or Wayland), this renders to a VkImage in device-local memory.
 *
 * Three modes of operation:
 *
 * 1. DMA-BUF Export Mode (preferred, zero-copy):
 *    - Creates VkImage with VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
 *    - Exports as DMA-BUF fd
 *    - Host imports via EGL_EXT_image_dma_buf_import (OpenGL) or
 *      VK_EXT_external_memory_dma_buf (Vulkan)
 *    - Requires VK_EXT_external_memory_dma_buf + VK_KHR_external_memory_fd
 *
 * 2. CPU Readback Mode (fallback, always works):
 *    - Renders to VkImage in device-local memory
 *    - Copies to host-visible staging buffer
 *    - Maps staging buffer and passes pixel data to callback
 *    - Host uploads to its own texture
 *
 * 3. External Swapchain Mode (Vulkan-to-Vulkan, most efficient):
 *    - For hosts that also use Vulkan (e.g., Flutter with Impeller)
 *    - Uses VK_KHR_external_memory_fd to share VkDeviceMemory
 *    - Host imports the external memory and creates its own VkImage
 *    - Synchronization via VK_KHR_external_semaphore_fd
 *
 * This class integrates with Godot's existing RenderingContextDriver system
 * by creating a "headless" Vulkan context that does NOT require a VkSurfaceKHR.
 * Instead, it implements a custom swapchain-like mechanism using offscreen images.
 */
class RenderingNativeSurfaceLinuxVulkan : public RenderingNativeSurface {
	GDCLASS(RenderingNativeSurfaceLinuxVulkan, RenderingNativeSurface);

	static void _bind_methods();

public:
	enum Mode {
		MODE_DMABUF_EXPORT = 0,
		MODE_CPU_READBACK = 1,
		MODE_EXTERNAL_SWAPCHAIN = 2,
	};

private:
	Mode mode = MODE_CPU_READBACK;

	uint32_t width = 0;
	uint32_t height = 0;

	// Vulkan instance and device (managed by the RenderingContextDriver).
	// We store these for convenience in frame readback / export operations.
	VkInstance vk_instance = VK_NULL_HANDLE;
	VkDevice vk_device = VK_NULL_HANDLE;
	VkPhysicalDevice vk_physical_device = VK_NULL_HANDLE;
	VkQueue vk_queue = VK_NULL_HANDLE;
	uint32_t queue_family_index = 0;

	// Offscreen render target.
	// These are the images Godot renders into instead of a swapchain.
	static const uint32_t SWAPCHAIN_IMAGE_COUNT = 2;
	struct SwapchainImage {
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		int dmabuf_fd = -1; // Only in DMA-BUF mode
	};
	SwapchainImage swapchain_images[SWAPCHAIN_IMAGE_COUNT];
	uint32_t current_image_index = 0;

	// Staging buffer for CPU readback.
	VkBuffer staging_buffer = VK_NULL_HANDLE;
	VkDeviceMemory staging_memory = VK_NULL_HANDLE;
	void *staging_mapped = nullptr;
	VkDeviceSize staging_buffer_size = 0;

	// Synchronization.
	VkSemaphore render_finished_semaphore = VK_NULL_HANDLE;
	VkFence frame_fence = VK_NULL_HANDLE;
	VkCommandPool command_pool = VK_NULL_HANDLE;
	VkCommandBuffer copy_command_buffer = VK_NULL_HANDLE;

	// External synchronization (for external swapchain mode).
	VkSemaphore external_semaphore = VK_NULL_HANDLE;
	int external_semaphore_fd = -1;

	// Callbacks.
	GodotVulkanFrameReadyCallback readback_callback = nullptr;
	void *readback_callback_userdata = nullptr;
	GodotVulkanDmaBufReadyCallback dmabuf_callback = nullptr;
	void *dmabuf_callback_userdata = nullptr;

	// Internal methods.
	Error _create_offscreen_images();
	Error _create_staging_buffer();
	Error _create_command_pool();
	Error _create_sync_objects();
	void _destroy_offscreen_images();
	void _destroy_staging_buffer();

	// Helper: find memory type index.
	uint32_t _find_memory_type(uint32_t p_type_filter, VkMemoryPropertyFlags p_properties) const;

	// Helper: export DMA-BUF fd from VkDeviceMemory.
	int _export_dmabuf_fd(VkDeviceMemory p_memory) const;

public:
	/**
	 * Create in CPU readback mode (simplest, always works).
	 */
	static Ref<RenderingNativeSurfaceLinuxVulkan> create_readback(
			uint32_t p_width, uint32_t p_height);

	/**
	 * Create in DMA-BUF export mode (zero-copy, requires extensions).
	 */
	static Ref<RenderingNativeSurfaceLinuxVulkan> create_dmabuf(
			uint32_t p_width, uint32_t p_height);

	/**
	 * GDExtension-compatible factory.
	 */
	static Ref<RenderingNativeSurfaceLinuxVulkan> create_api(
			uint32_t p_width, uint32_t p_height, int p_mode);

	// Post-initialization: set the Vulkan device handles.
	// Called after the RenderingContextDriver has initialized.
	void set_vulkan_handles(VkInstance p_instance, VkPhysicalDevice p_physical_device,
			VkDevice p_device, VkQueue p_queue, uint32_t p_queue_family_index);

	// Initialize offscreen rendering resources.
	Error initialize_offscreen();

	// Get current render target image (for Godot to render into).
	VkImage get_current_image() const;
	VkImageView get_current_image_view() const;
	uint32_t get_current_image_index() const { return current_image_index; }

	// Advance to next image (like vkAcquireNextImageKHR).
	void acquire_next_image();

	// Present the rendered frame (like vkQueuePresentKHR).
	// Copies to staging buffer (readback mode) or signals DMA-BUF ready.
	Error present_frame();

	// Get dimensions.
	uint32_t get_width() const { return width; }
	uint32_t get_height() const { return height; }

	// Resize.
	void resize(uint32_t p_width, uint32_t p_height);

	// Set callbacks.
	void set_readback_callback(GodotVulkanFrameReadyCallback p_callback, void *p_userdata);
	void set_dmabuf_callback(GodotVulkanDmaBufReadyCallback p_callback, void *p_userdata);

	// Get DMA-BUF fd for current front image.
	int get_current_dmabuf_fd() const;

	// Get external semaphore fd (for external swapchain sync).
	int get_external_semaphore_fd() const { return external_semaphore_fd; }

	// Get mode.
	Mode get_mode() const { return mode; }

	// RenderingNativeSurface interface.
	RenderingContextDriver *create_rendering_context() override;

	RenderingNativeSurfaceLinuxVulkan();
	~RenderingNativeSurfaceLinuxVulkan();
};

VARIANT_ENUM_CAST(RenderingNativeSurfaceLinuxVulkan::Mode);

#endif // VULKAN_ENABLED

#endif // RENDERING_NATIVE_SURFACE_LINUX_VULKAN_H
