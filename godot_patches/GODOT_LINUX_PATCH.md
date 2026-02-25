# Godot Linux Embedded Rendering Patch

## Overview

This patch adds Linux-specific RenderingNativeSurface implementations to
Migeran's LibGodot fork, enabling Godot to render offscreen on Linux for
embedding in host applications like Flutter, Qt, or Electron.

The existing LibGodot/DisplayServerEmbedded infrastructure has platform-specific
implementations for iOS (RenderingNativeSurfaceApple) and Android
(RenderingNativeSurfaceAndroid), but nothing for Linux. This patch fills that gap.

## Architecture

### The Problem

When Godot runs as a library (libgodot.so) on Linux, it uses
`DisplayServerEmbedded` instead of the normal X11/Wayland display servers.
The embedded display server requires a `RenderingNativeSurface` to create
rendering contexts and windows. On iOS this wraps a CALayer; on Android,
an ANativeWindow. On Linux, there was no equivalent -- until now.

### The Solution

Two new `RenderingNativeSurface` subclasses for Linux:

1. **RenderingNativeSurfaceLinuxEGL** (OpenGL path)
   - Creates an offscreen EGL context with PbufferSurface
   - Renders to an FBO with double-buffered textures
   - Three modes:
     - **Shared Context**: Host provides EGLDisplay/EGLContext; Godot creates
       a shared context so GL textures are visible in both
     - **Pixel Readback**: glReadPixels fallback that always works
     - **DMA-BUF Export**: Zero-copy via EGL_EXT_image_dma_buf_import (future)

2. **RenderingNativeSurfaceLinuxVulkan** (Vulkan path)
   - Creates headless Vulkan rendering without VkSurfaceKHR
   - Custom "swapchain" using offscreen VkImages
   - Three modes:
     - **CPU Readback**: Copy to staging buffer, map, pass pixels to host
     - **DMA-BUF Export**: Export VkDeviceMemory as DMA-BUF fd via
       VK_KHR_external_memory_fd + VK_EXT_external_memory_dma_buf
     - **External Swapchain**: Share VkDeviceMemory fd + VkSemaphore fd
       for Vulkan-to-Vulkan zero-copy

### Integration with Flutter

The typical Flutter integration flow:

```
Flutter App
    |
    +-- Native Plugin (C/C++)
    |       |
    |       +-- libgodot.so (Migeran's fork + this patch)
    |       |       |
    |       |       +-- DisplayServerEmbedded
    |       |       |       |
    |       |       |       +-- RenderingNativeSurfaceLinuxEGL (GL)
    |       |       |       |   or
    |       |       |       +-- RenderingNativeSurfaceLinuxVulkan (Vk)
    |       |       |
    |       |       +-- Godot game scene renders to offscreen FBO/VkImage
    |       |
    |       +-- Copies frame pixels to FlPixelBufferTexture
    |           (or shares GL texture via FlTextureGL)
    |
    +-- Dart Texture widget displays the result
```

### The Rendering Pipeline

#### OpenGL (EGL) Path

```
1. Host creates RenderingNativeSurfaceLinuxEGL::create_shared(display, context, w, h)
2. DisplayServerEmbedded::set_native_surface(surface)
3. libgodot starts, DisplayServerEmbedded uses the surface
4. Each frame:
   a. surface->make_current()  -- bind Godot's EGL context
   b. glBindFramebuffer(GL_FRAMEBUFFER, surface->get_fbo())
   c. Godot renders scene
   d. surface->present_frame()  -- readback or texture swap
   e. Host reads surface->get_front_texture() or get_pixel_data()
```

#### Vulkan Path

```
1. Host creates RenderingNativeSurfaceLinuxVulkan::create_readback(w, h)
2. surface creates RenderingContextDriverVulkanLinuxHeadless
3. Headless driver creates VkInstance without platform surface extension
4. surface->set_vulkan_handles(instance, physdev, device, queue, family)
5. surface->initialize_offscreen()  -- creates VkImages, staging buffer
6. Each frame:
   a. Godot renders to surface->get_current_image()
   b. surface->present_frame()  -- copies to staging, invokes callback
   c. Host receives pixel data via callback
```

## Files Added

```
platform/linuxbsd/embedded/
    rendering_native_surface_linux_egl.h
    rendering_native_surface_linux_egl.cpp
    rendering_native_surface_linux_vulkan.h
    rendering_native_surface_linux_vulkan.cpp
    SCsub
```

## Files Modified

```
platform/linuxbsd/SCsub
    -- Add embedded/ subdirectory for library builds

platform/linuxbsd/api/api.cpp
    -- Register RenderingNativeSurfaceLinuxEGL and
       RenderingNativeSurfaceLinuxVulkan classes

servers/display_server_embedded.cpp
    -- Remove Apple-only include
    -- Enable OpenGL3 driver on Linux
    -- Add "opengl3" to rendering drivers list on Linux
```

## Prerequisites

- Migeran's LibGodot fork: https://github.com/nickel-lang/nickel
  Branch: `libgodot_migeran_45`
- Vulkan SDK (for Vulkan path)
- EGL + GLES3 headers (for OpenGL path)
- GBM + DRM headers (for DMA-BUF export, optional)

```bash
# Ubuntu/Debian
sudo apt install -y \
    build-essential scons pkg-config \
    libx11-dev libxcursor-dev libxinerama-dev libxrandr-dev libxrender-dev \
    libxi-dev libgl-dev libegl-dev libgles-dev \
    libvulkan-dev vulkan-validationlayers \
    libgbm-dev libdrm-dev \
    libasound2-dev libpulse-dev \
    libdbus-1-dev libfontconfig-dev libfreetype-dev \
    libspeechd-dev libudev-dev
```

## How to Apply the Patch

### Step 1: Clone Migeran's fork

```bash
git clone https://github.com/nickel-lang/nickel.git --branch libgodot_migeran_45 godot
cd godot
```

### Step 2: Copy the new files

```bash
mkdir -p platform/linuxbsd/embedded

cp /path/to/godot_patches/platform/linuxbsd/embedded/rendering_native_surface_linux_egl.h \
   platform/linuxbsd/embedded/
cp /path/to/godot_patches/platform/linuxbsd/embedded/rendering_native_surface_linux_egl.cpp \
   platform/linuxbsd/embedded/
cp /path/to/godot_patches/platform/linuxbsd/embedded/rendering_native_surface_linux_vulkan.h \
   platform/linuxbsd/embedded/
cp /path/to/godot_patches/platform/linuxbsd/embedded/rendering_native_surface_linux_vulkan.cpp \
   platform/linuxbsd/embedded/
cp /path/to/godot_patches/platform/linuxbsd/embedded/SCsub \
   platform/linuxbsd/embedded/
```

### Step 3: Apply modifications to existing files

Either apply the patch file:

```bash
git apply /path/to/godot_patches/godot_linux_embedded.patch
```

Or make the changes manually (see "Files Modified" above for details).

### Step 4: Build as shared library

```bash
# Vulkan-only build (recommended for first test)
scons platform=linuxbsd target=template_release \
    library_type=shared_library \
    vulkan=yes opengl3=no \
    use_sowrap=yes \
    -j$(nproc)

# OpenGL + Vulkan build
scons platform=linuxbsd target=template_release \
    library_type=shared_library \
    vulkan=yes opengl3=yes \
    use_sowrap=yes \
    -j$(nproc)

# Editor build (for debugging)
scons platform=linuxbsd target=editor dev_build=yes \
    library_type=shared_library \
    vulkan=yes opengl3=yes \
    use_sowrap=yes \
    -j$(nproc)
```

The output will be:
- `bin/libgodot.linuxbsd.template_release.x86_64.so` (release)
- `bin/libgodot.linuxbsd.editor.dev.x86_64.so` (debug)

### Step 5: Test

Create a minimal C test program:

```c
// test_embedded.c
#include <stdio.h>
#include <dlfcn.h>

// LibGodot entry points
typedef void* (*CreateInstanceFn)(int argc, char** argv, void* init_func);
typedef void (*DestroyInstanceFn)(void* instance);

int main(int argc, char** argv) {
    void* lib = dlopen("./libgodot.linuxbsd.template_release.x86_64.so", RTLD_NOW);
    if (!lib) {
        fprintf(stderr, "Failed to load: %s\n", dlerror());
        return 1;
    }

    CreateInstanceFn create = dlsym(lib, "libgodot_create_godot_instance");
    DestroyInstanceFn destroy = dlsym(lib, "libgodot_destroy_godot_instance");

    if (!create || !destroy) {
        fprintf(stderr, "Missing symbols\n");
        return 1;
    }

    printf("LibGodot loaded successfully!\n");
    printf("  create: %p\n", create);
    printf("  destroy: %p\n", destroy);

    // In a real integration, you would:
    // 1. Create a RenderingNativeSurfaceLinuxVulkan or LinuxEGL
    // 2. Call DisplayServerEmbedded::set_native_surface()
    // 3. Create the Godot instance
    // 4. Run the game loop, reading frames from the surface

    dlclose(lib);
    return 0;
}
```

Compile and run:

```bash
gcc -o test_embedded test_embedded.c -ldl
./test_embedded
```

## Integration Guide for Flutter

### Using the OpenGL/EGL Path (Recommended for Flutter)

Flutter's Linux embedder uses OpenGL ES via EGL. The integration steps:

1. **In your Flutter native plugin (C++):**

```cpp
#include "rendering_native_surface_linux_egl.h"

// Get Flutter's EGL display and context
EGLDisplay flutter_display = eglGetCurrentDisplay();
EGLContext flutter_context = eglGetCurrentContext();

// Create a shared surface
auto surface = RenderingNativeSurfaceLinuxEGL::create_shared(
    flutter_display, flutter_context, 800, 600);
surface->initialize();

// Set as Godot's native surface BEFORE creating the instance
DisplayServerEmbedded::set_native_surface(surface);

// Create Godot instance...
// After each Godot frame:
surface->present_frame();
GLuint texture = surface->get_front_texture();
// Register this texture with Flutter's texture registry
```

2. **For pixel buffer approach (simpler, no context sharing needed):**

```cpp
auto surface = RenderingNativeSurfaceLinuxEGL::create_offscreen(800, 600);
surface->initialize();

// After each Godot frame:
surface->present_frame();
const uint8_t* pixels = surface->get_pixel_data();
// Copy to FlPixelBufferTexture
```

### Using the Vulkan Path

If Flutter uses Impeller with Vulkan backend:

```cpp
auto surface = RenderingNativeSurfaceLinuxVulkan::create_readback(800, 600);
// ... Godot initializes Vulkan internally ...

surface->set_readback_callback([](const uint8_t* pixels,
    uint32_t w, uint32_t h, uint32_t stride, void* ud) {
    // Copy pixels to Flutter texture
}, nullptr);
```

## Known Limitations

1. **OpenGL path is not yet wired into DisplayServerEmbedded's Vulkan-centric
   initialization flow.** The embedded display server currently only initializes
   through the RD (RenderingDevice) path which expects Vulkan. For OpenGL,
   the surface must be used directly, bypassing DisplayServerEmbedded's
   constructor. This will require additional work in Godot's GLES3 renderer
   to support offscreen FBO targets.

2. **DMA-BUF export requires specific GPU driver support.** Not all drivers
   support VK_EXT_external_memory_dma_buf or EGL_EXT_image_dma_buf_import.
   The CPU readback path is the universal fallback.

3. **The headless Vulkan context driver needs more work.** The
   RenderingContextDriverVulkanLinuxHeadless bypasses the swapchain entirely,
   which means Godot's RenderingDevice needs to be adapted to render to
   our offscreen images instead of swapchain images. This is the most complex
   integration point and may require changes to rendering_device_vulkan.cpp.

4. **Thread safety**: Godot's rendering must happen on a dedicated thread,
   separate from Flutter's UI thread. The frame readback/texture sharing must
   be properly synchronized.

## Future Work

- Integrate with Godot's RenderingDevice to use offscreen images as
  swapchain replacements
- Add GBM-based EGL context creation for truly headless (no X11/Wayland) operation
- Implement VK_KHR_external_semaphore_fd for Vulkan-to-Vulkan sync
- Add resize handling that integrates with DisplayServerEmbedded::resize_window()
- Explore PBO (Pixel Buffer Objects) for async pixel readback on the GL path
