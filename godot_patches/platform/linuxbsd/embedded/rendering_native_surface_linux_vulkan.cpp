/**************************************************************************/
/*  rendering_native_surface_linux_vulkan.cpp                             */
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

#ifdef VULKAN_ENABLED

#include "rendering_native_surface_linux_vulkan.h"

#include "core/error/error_macros.h"
#include "core/os/memory.h"
#include "core/string/print_string.h"

#include "drivers/vulkan/rendering_context_driver_vulkan.h"

#include <cstring>

// ============================================================================
// Headless Vulkan rendering context driver for Linux embedded
// ============================================================================

/**
 * RenderingContextDriverVulkanLinuxHeadless
 *
 * A Vulkan rendering context driver that does NOT require a VkSurfaceKHR.
 * Instead of creating a surface from a window, it creates offscreen VkImages
 * and manages a custom "swapchain" for headless rendering.
 *
 * This is the key piece that makes embedded Vulkan rendering work on Linux:
 * Godot's normal Vulkan path requires a window surface for presentation.
 * This driver bypasses that by providing offscreen images that can be exported
 * via DMA-BUF or read back to CPU memory.
 */
class RenderingContextDriverVulkanLinuxHeadless : public RenderingContextDriverVulkan {
private:
	RenderingNativeSurfaceLinuxVulkan *owner = nullptr;

	virtual const char *_get_platform_surface_extension() const override {
		// We don't need a platform surface extension because we're not
		// creating VkSurfaceKHR objects. Return nullptr to skip the
		// platform surface extension requirement.
		return nullptr;
	}

protected:
	SurfaceID surface_create(Ref<RenderingNativeSurface> p_native_surface) override {
		// For headless rendering, we create a "virtual" surface that
		// represents our offscreen render target.
		// The RenderingContextDriverVulkan::Surface struct stores a VkSurfaceKHR,
		// but since we're headless, we won't have one.
		//
		// We need to provide a SurfaceID that the rest of Godot's rendering
		// pipeline can work with. We'll create a Surface with a null VkSurfaceKHR
		// and override the width/height to match our offscreen target.

		Ref<RenderingNativeSurfaceLinuxVulkan> linux_surface =
				Object::cast_to<RenderingNativeSurfaceLinuxVulkan>(*p_native_surface);
		ERR_FAIL_COND_V(linux_surface.is_null(), SurfaceID());

		Surface *surface = memnew(Surface);
		surface->vk_surface = VK_NULL_HANDLE; // No window surface!
		surface->width = linux_surface->get_width();
		surface->height = linux_surface->get_height();
		surface->needs_resize = false;
		return SurfaceID(surface);
	}

public:
	RenderingContextDriverVulkanLinuxHeadless(RenderingNativeSurfaceLinuxVulkan *p_owner)
			: owner(p_owner) {
	}

	~RenderingContextDriverVulkanLinuxHeadless() override {
	}
};

// ============================================================================
// Bind methods
// ============================================================================

void RenderingNativeSurfaceLinuxVulkan::_bind_methods() {
	ClassDB::bind_static_method("RenderingNativeSurfaceLinuxVulkan",
			D_METHOD("create", "width", "height", "mode"),
			&RenderingNativeSurfaceLinuxVulkan::create_api);

	BIND_ENUM_CONSTANT(MODE_DMABUF_EXPORT);
	BIND_ENUM_CONSTANT(MODE_CPU_READBACK);
	BIND_ENUM_CONSTANT(MODE_EXTERNAL_SWAPCHAIN);
}

// ============================================================================
// Static factory methods
// ============================================================================

Ref<RenderingNativeSurfaceLinuxVulkan> RenderingNativeSurfaceLinuxVulkan::create_readback(
		uint32_t p_width, uint32_t p_height) {
	Ref<RenderingNativeSurfaceLinuxVulkan> result = memnew(RenderingNativeSurfaceLinuxVulkan);
	result->mode = MODE_CPU_READBACK;
	result->width = p_width;
	result->height = p_height;
	return result;
}

Ref<RenderingNativeSurfaceLinuxVulkan> RenderingNativeSurfaceLinuxVulkan::create_dmabuf(
		uint32_t p_width, uint32_t p_height) {
	Ref<RenderingNativeSurfaceLinuxVulkan> result = memnew(RenderingNativeSurfaceLinuxVulkan);
	result->mode = MODE_DMABUF_EXPORT;
	result->width = p_width;
	result->height = p_height;
	return result;
}

Ref<RenderingNativeSurfaceLinuxVulkan> RenderingNativeSurfaceLinuxVulkan::create_api(
		uint32_t p_width, uint32_t p_height, int p_mode) {
	Ref<RenderingNativeSurfaceLinuxVulkan> result = memnew(RenderingNativeSurfaceLinuxVulkan);
	result->mode = (Mode)p_mode;
	result->width = p_width;
	result->height = p_height;
	return result;
}

// ============================================================================
// Vulkan handle setup
// ============================================================================

void RenderingNativeSurfaceLinuxVulkan::set_vulkan_handles(
		VkInstance p_instance, VkPhysicalDevice p_physical_device,
		VkDevice p_device, VkQueue p_queue, uint32_t p_queue_family_index) {
	vk_instance = p_instance;
	vk_physical_device = p_physical_device;
	vk_device = p_device;
	vk_queue = p_queue;
	queue_family_index = p_queue_family_index;
}

// ============================================================================
// Offscreen rendering initialization
// ============================================================================

Error RenderingNativeSurfaceLinuxVulkan::initialize_offscreen() {
	ERR_FAIL_COND_V_MSG(vk_device == VK_NULL_HANDLE, ERR_UNCONFIGURED,
			"Vulkan device not set. Call set_vulkan_handles() first.");

	Error err;

	err = _create_command_pool();
	ERR_FAIL_COND_V(err != OK, err);

	err = _create_offscreen_images();
	ERR_FAIL_COND_V(err != OK, err);

	if (mode == MODE_CPU_READBACK) {
		err = _create_staging_buffer();
		ERR_FAIL_COND_V(err != OK, err);
	}

	err = _create_sync_objects();
	ERR_FAIL_COND_V(err != OK, err);

	print_verbose(vformat("RenderingNativeSurfaceLinuxVulkan: Initialized offscreen rendering (%dx%d, mode=%d).",
			width, height, (int)mode));

	return OK;
}

// ============================================================================
// Offscreen image creation
// ============================================================================

Error RenderingNativeSurfaceLinuxVulkan::_create_offscreen_images() {
	VkFormat format = VK_FORMAT_B8G8R8A8_UNORM; // Standard desktop format

	for (uint32_t i = 0; i < SWAPCHAIN_IMAGE_COUNT; i++) {
		VkImageCreateInfo image_info = {};
		image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		image_info.imageType = VK_IMAGE_TYPE_2D;
		image_info.format = format;
		image_info.extent.width = width;
		image_info.extent.height = height;
		image_info.extent.depth = 1;
		image_info.mipLevels = 1;
		image_info.arrayLayers = 1;
		image_info.samples = VK_SAMPLE_COUNT_1_BIT;
		image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
		image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
				VK_IMAGE_USAGE_TRANSFER_SRC_BIT | // For readback
				VK_IMAGE_USAGE_SAMPLED_BIT;       // For potential sampling
		image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		// For DMA-BUF export mode, add external memory info.
		VkExternalMemoryImageCreateInfo external_info = {};
		if (mode == MODE_DMABUF_EXPORT) {
			external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
			external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
			image_info.pNext = &external_info;
			// DMA-BUF requires linear tiling for cross-device compatibility.
			image_info.tiling = VK_IMAGE_TILING_LINEAR;
		}

		VkResult res = vkCreateImage(vk_device, &image_info, nullptr, &swapchain_images[i].image);
		ERR_FAIL_COND_V_MSG(res != VK_SUCCESS, ERR_CANT_CREATE,
				vformat("Failed to create offscreen image %d: VkResult %d", i, (int)res));

		// Allocate memory.
		VkMemoryRequirements mem_reqs;
		vkGetImageMemoryRequirements(vk_device, swapchain_images[i].image, &mem_reqs);

		VkMemoryAllocateInfo alloc_info = {};
		alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		alloc_info.allocationSize = mem_reqs.size;

		VkMemoryPropertyFlags mem_props = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

		// For DMA-BUF export, we need exportable memory.
		VkExportMemoryAllocateInfo export_alloc_info = {};
		if (mode == MODE_DMABUF_EXPORT) {
			export_alloc_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
			export_alloc_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
			alloc_info.pNext = &export_alloc_info;
		}

		alloc_info.memoryTypeIndex = _find_memory_type(mem_reqs.memoryTypeBits, mem_props);
		ERR_FAIL_COND_V_MSG(alloc_info.memoryTypeIndex == UINT32_MAX, ERR_CANT_CREATE,
				"Failed to find suitable memory type for offscreen image.");

		res = vkAllocateMemory(vk_device, &alloc_info, nullptr, &swapchain_images[i].memory);
		ERR_FAIL_COND_V_MSG(res != VK_SUCCESS, ERR_CANT_CREATE,
				vformat("Failed to allocate memory for offscreen image %d.", i));

		res = vkBindImageMemory(vk_device, swapchain_images[i].image, swapchain_images[i].memory, 0);
		ERR_FAIL_COND_V_MSG(res != VK_SUCCESS, ERR_CANT_CREATE,
				vformat("Failed to bind memory for offscreen image %d.", i));

		// Create image view.
		VkImageViewCreateInfo view_info = {};
		view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view_info.image = swapchain_images[i].image;
		view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view_info.format = format;
		view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		view_info.subresourceRange.baseMipLevel = 0;
		view_info.subresourceRange.levelCount = 1;
		view_info.subresourceRange.baseArrayLayer = 0;
		view_info.subresourceRange.layerCount = 1;

		res = vkCreateImageView(vk_device, &view_info, nullptr, &swapchain_images[i].view);
		ERR_FAIL_COND_V_MSG(res != VK_SUCCESS, ERR_CANT_CREATE,
				vformat("Failed to create image view for offscreen image %d.", i));

		// Export DMA-BUF fd if in export mode.
		if (mode == MODE_DMABUF_EXPORT) {
			swapchain_images[i].dmabuf_fd = _export_dmabuf_fd(swapchain_images[i].memory);
			if (swapchain_images[i].dmabuf_fd < 0) {
				WARN_PRINT(vformat("Failed to export DMA-BUF fd for image %d. Falling back to readback.", i));
				mode = MODE_CPU_READBACK;
			}
		}
	}

	return OK;
}

Error RenderingNativeSurfaceLinuxVulkan::_create_staging_buffer() {
	VkDeviceSize buffer_size = (VkDeviceSize)width * height * 4; // BGRA8

	VkBufferCreateInfo buffer_info = {};
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size = buffer_size;
	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VkResult res = vkCreateBuffer(vk_device, &buffer_info, nullptr, &staging_buffer);
	ERR_FAIL_COND_V_MSG(res != VK_SUCCESS, ERR_CANT_CREATE, "Failed to create staging buffer.");

	VkMemoryRequirements mem_reqs;
	vkGetBufferMemoryRequirements(vk_device, staging_buffer, &mem_reqs);

	VkMemoryAllocateInfo alloc_info = {};
	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.allocationSize = mem_reqs.size;
	alloc_info.memoryTypeIndex = _find_memory_type(mem_reqs.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	ERR_FAIL_COND_V_MSG(alloc_info.memoryTypeIndex == UINT32_MAX, ERR_CANT_CREATE,
			"Failed to find host-visible memory type for staging buffer.");

	res = vkAllocateMemory(vk_device, &alloc_info, nullptr, &staging_memory);
	ERR_FAIL_COND_V_MSG(res != VK_SUCCESS, ERR_CANT_CREATE, "Failed to allocate staging buffer memory.");

	res = vkBindBufferMemory(vk_device, staging_buffer, staging_memory, 0);
	ERR_FAIL_COND_V_MSG(res != VK_SUCCESS, ERR_CANT_CREATE, "Failed to bind staging buffer memory.");

	res = vkMapMemory(vk_device, staging_memory, 0, buffer_size, 0, &staging_mapped);
	ERR_FAIL_COND_V_MSG(res != VK_SUCCESS, ERR_CANT_CREATE, "Failed to map staging buffer memory.");

	staging_buffer_size = buffer_size;

	return OK;
}

Error RenderingNativeSurfaceLinuxVulkan::_create_command_pool() {
	VkCommandPoolCreateInfo pool_info = {};
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = queue_family_index;

	VkResult res = vkCreateCommandPool(vk_device, &pool_info, nullptr, &command_pool);
	ERR_FAIL_COND_V_MSG(res != VK_SUCCESS, ERR_CANT_CREATE, "Failed to create command pool.");

	VkCommandBufferAllocateInfo alloc_info = {};
	alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	alloc_info.commandPool = command_pool;
	alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc_info.commandBufferCount = 1;

	res = vkAllocateCommandBuffers(vk_device, &alloc_info, &copy_command_buffer);
	ERR_FAIL_COND_V_MSG(res != VK_SUCCESS, ERR_CANT_CREATE, "Failed to allocate copy command buffer.");

	return OK;
}

Error RenderingNativeSurfaceLinuxVulkan::_create_sync_objects() {
	VkSemaphoreCreateInfo sem_info = {};
	sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VkResult res = vkCreateSemaphore(vk_device, &sem_info, nullptr, &render_finished_semaphore);
	ERR_FAIL_COND_V_MSG(res != VK_SUCCESS, ERR_CANT_CREATE, "Failed to create render finished semaphore.");

	VkFenceCreateInfo fence_info = {};
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT; // Start signaled

	res = vkCreateFence(vk_device, &fence_info, nullptr, &frame_fence);
	ERR_FAIL_COND_V_MSG(res != VK_SUCCESS, ERR_CANT_CREATE, "Failed to create frame fence.");

	// Create exportable semaphore for external swapchain mode.
	if (mode == MODE_EXTERNAL_SWAPCHAIN) {
		VkExportSemaphoreCreateInfo export_sem_info = {};
		export_sem_info.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
		export_sem_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

		VkSemaphoreCreateInfo ext_sem_info = {};
		ext_sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		ext_sem_info.pNext = &export_sem_info;

		res = vkCreateSemaphore(vk_device, &ext_sem_info, nullptr, &external_semaphore);
		if (res == VK_SUCCESS) {
			// Export the semaphore fd.
			VkSemaphoreGetFdInfoKHR fd_info = {};
			fd_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
			fd_info.semaphore = external_semaphore;
			fd_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

			PFN_vkGetSemaphoreFdKHR vkGetSemaphoreFdKHR =
					(PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(vk_device, "vkGetSemaphoreFdKHR");
			if (vkGetSemaphoreFdKHR) {
				vkGetSemaphoreFdKHR(vk_device, &fd_info, &external_semaphore_fd);
			}
		}
	}

	return OK;
}

// ============================================================================
// Frame operations
// ============================================================================

VkImage RenderingNativeSurfaceLinuxVulkan::get_current_image() const {
	return swapchain_images[current_image_index].image;
}

VkImageView RenderingNativeSurfaceLinuxVulkan::get_current_image_view() const {
	return swapchain_images[current_image_index].view;
}

void RenderingNativeSurfaceLinuxVulkan::acquire_next_image() {
	current_image_index = (current_image_index + 1) % SWAPCHAIN_IMAGE_COUNT;
}

Error RenderingNativeSurfaceLinuxVulkan::present_frame() {
	ERR_FAIL_COND_V(vk_device == VK_NULL_HANDLE, ERR_UNCONFIGURED);

	// Wait for previous frame to finish.
	vkWaitForFences(vk_device, 1, &frame_fence, VK_TRUE, UINT64_MAX);
	vkResetFences(vk_device, 1, &frame_fence);

	uint32_t presented_index = current_image_index;

	if (mode == MODE_CPU_READBACK && staging_buffer != VK_NULL_HANDLE) {
		// Record and submit a command to copy the image to the staging buffer.
		VkCommandBufferBeginInfo begin_info = {};
		begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

		vkResetCommandBuffer(copy_command_buffer, 0);
		vkBeginCommandBuffer(copy_command_buffer, &begin_info);

		// Transition image to TRANSFER_SRC_OPTIMAL.
		VkImageMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.image = swapchain_images[presented_index].image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(copy_command_buffer,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				0, 0, nullptr, 0, nullptr, 1, &barrier);

		// Copy image to staging buffer.
		VkBufferImageCopy copy_region = {};
		copy_region.bufferOffset = 0;
		copy_region.bufferRowLength = 0; // Tightly packed
		copy_region.bufferImageHeight = 0;
		copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy_region.imageSubresource.mipLevel = 0;
		copy_region.imageSubresource.baseArrayLayer = 0;
		copy_region.imageSubresource.layerCount = 1;
		copy_region.imageOffset = { 0, 0, 0 };
		copy_region.imageExtent = { width, height, 1 };

		vkCmdCopyImageToBuffer(copy_command_buffer,
				swapchain_images[presented_index].image,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				staging_buffer, 1, &copy_region);

		// Transition image back to COLOR_ATTACHMENT_OPTIMAL for next frame.
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		vkCmdPipelineBarrier(copy_command_buffer,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				0, 0, nullptr, 0, nullptr, 1, &barrier);

		vkEndCommandBuffer(copy_command_buffer);

		// Submit.
		VkSubmitInfo submit_info = {};
		submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit_info.commandBufferCount = 1;
		submit_info.pCommandBuffers = &copy_command_buffer;

		vkQueueSubmit(vk_queue, 1, &submit_info, frame_fence);

		// Wait for the copy to complete, then invoke callback.
		vkWaitForFences(vk_device, 1, &frame_fence, VK_TRUE, UINT64_MAX);

		if (readback_callback && staging_mapped) {
			readback_callback(
					(const uint8_t *)staging_mapped,
					width, height,
					width * 4, // stride
					readback_callback_userdata);
		}
	} else if (mode == MODE_DMABUF_EXPORT) {
		// DMA-BUF mode: the image is already in GPU memory accessible via fd.
		// Just invoke the callback with the fd.
		if (dmabuf_callback) {
			int fd = swapchain_images[presented_index].dmabuf_fd;
			dmabuf_callback(
					fd,
					width, height,
					width * 4, // stride (for linear tiling)
					0x34325241, // DRM_FORMAT_BGRA8888
					0,         // DRM_FORMAT_MOD_LINEAR
					dmabuf_callback_userdata);
		}
	}

	// Advance to next image for the next frame.
	acquire_next_image();

	return OK;
}

// ============================================================================
// Resize
// ============================================================================

void RenderingNativeSurfaceLinuxVulkan::resize(uint32_t p_width, uint32_t p_height) {
	if (p_width == width && p_height == height) {
		return;
	}

	width = p_width;
	height = p_height;

	if (vk_device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(vk_device);
		_destroy_offscreen_images();
		_destroy_staging_buffer();
		_create_offscreen_images();
		if (mode == MODE_CPU_READBACK) {
			_create_staging_buffer();
		}
	}
}

// ============================================================================
// Callbacks
// ============================================================================

void RenderingNativeSurfaceLinuxVulkan::set_readback_callback(
		GodotVulkanFrameReadyCallback p_callback, void *p_userdata) {
	readback_callback = p_callback;
	readback_callback_userdata = p_userdata;
}

void RenderingNativeSurfaceLinuxVulkan::set_dmabuf_callback(
		GodotVulkanDmaBufReadyCallback p_callback, void *p_userdata) {
	dmabuf_callback = p_callback;
	dmabuf_callback_userdata = p_userdata;
}

int RenderingNativeSurfaceLinuxVulkan::get_current_dmabuf_fd() const {
	if (mode != MODE_DMABUF_EXPORT) {
		return -1;
	}
	// Return the fd of the PREVIOUS image (the one that was just rendered).
	uint32_t prev = (current_image_index + SWAPCHAIN_IMAGE_COUNT - 1) % SWAPCHAIN_IMAGE_COUNT;
	return swapchain_images[prev].dmabuf_fd;
}

// ============================================================================
// Helper methods
// ============================================================================

uint32_t RenderingNativeSurfaceLinuxVulkan::_find_memory_type(
		uint32_t p_type_filter, VkMemoryPropertyFlags p_properties) const {
	VkPhysicalDeviceMemoryProperties mem_properties;
	vkGetPhysicalDeviceMemoryProperties(vk_physical_device, &mem_properties);

	for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
		if ((p_type_filter & (1 << i)) &&
				(mem_properties.memoryTypes[i].propertyFlags & p_properties) == p_properties) {
			return i;
		}
	}

	return UINT32_MAX;
}

int RenderingNativeSurfaceLinuxVulkan::_export_dmabuf_fd(VkDeviceMemory p_memory) const {
	PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR =
			(PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(vk_device, "vkGetMemoryFdKHR");
	if (!vkGetMemoryFdKHR) {
		ERR_PRINT("vkGetMemoryFdKHR not available. DMA-BUF export requires VK_KHR_external_memory_fd.");
		return -1;
	}

	VkMemoryGetFdInfoKHR fd_info = {};
	fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
	fd_info.memory = p_memory;
	fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

	int fd = -1;
	VkResult res = vkGetMemoryFdKHR(vk_device, &fd_info, &fd);
	if (res != VK_SUCCESS) {
		ERR_PRINT(vformat("vkGetMemoryFdKHR failed: %d", (int)res));
		return -1;
	}

	return fd;
}

// ============================================================================
// Cleanup
// ============================================================================

void RenderingNativeSurfaceLinuxVulkan::_destroy_offscreen_images() {
	for (uint32_t i = 0; i < SWAPCHAIN_IMAGE_COUNT; i++) {
		if (swapchain_images[i].view != VK_NULL_HANDLE) {
			vkDestroyImageView(vk_device, swapchain_images[i].view, nullptr);
			swapchain_images[i].view = VK_NULL_HANDLE;
		}
		if (swapchain_images[i].image != VK_NULL_HANDLE) {
			vkDestroyImage(vk_device, swapchain_images[i].image, nullptr);
			swapchain_images[i].image = VK_NULL_HANDLE;
		}
		if (swapchain_images[i].memory != VK_NULL_HANDLE) {
			vkFreeMemory(vk_device, swapchain_images[i].memory, nullptr);
			swapchain_images[i].memory = VK_NULL_HANDLE;
		}
		if (swapchain_images[i].dmabuf_fd >= 0) {
			// close(swapchain_images[i].dmabuf_fd);
			swapchain_images[i].dmabuf_fd = -1;
		}
	}
}

void RenderingNativeSurfaceLinuxVulkan::_destroy_staging_buffer() {
	if (staging_mapped) {
		vkUnmapMemory(vk_device, staging_memory);
		staging_mapped = nullptr;
	}
	if (staging_buffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(vk_device, staging_buffer, nullptr);
		staging_buffer = VK_NULL_HANDLE;
	}
	if (staging_memory != VK_NULL_HANDLE) {
		vkFreeMemory(vk_device, staging_memory, nullptr);
		staging_memory = VK_NULL_HANDLE;
	}
	staging_buffer_size = 0;
}

// ============================================================================
// RenderingNativeSurface interface
// ============================================================================

RenderingContextDriver *RenderingNativeSurfaceLinuxVulkan::create_rendering_context() {
	return memnew(RenderingContextDriverVulkanLinuxHeadless(this));
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

RenderingNativeSurfaceLinuxVulkan::RenderingNativeSurfaceLinuxVulkan() {
	// Does nothing.
}

RenderingNativeSurfaceLinuxVulkan::~RenderingNativeSurfaceLinuxVulkan() {
	if (vk_device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(vk_device);

		_destroy_offscreen_images();
		_destroy_staging_buffer();

		if (render_finished_semaphore != VK_NULL_HANDLE) {
			vkDestroySemaphore(vk_device, render_finished_semaphore, nullptr);
		}
		if (external_semaphore != VK_NULL_HANDLE) {
			vkDestroySemaphore(vk_device, external_semaphore, nullptr);
		}
		if (frame_fence != VK_NULL_HANDLE) {
			vkDestroyFence(vk_device, frame_fence, nullptr);
		}
		if (command_pool != VK_NULL_HANDLE) {
			vkDestroyCommandPool(vk_device, command_pool, nullptr);
		}
	}
}

#endif // VULKAN_ENABLED
