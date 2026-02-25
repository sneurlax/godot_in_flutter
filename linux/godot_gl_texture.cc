// godot_gl_texture.cc -- FlTextureGL subclass for Godot embedded OpenGL rendering
//
// Implements double-buffered FBO rendering with a shared GDK GL context.
// Godot renders to the back FBO, populate() returns the front texture.
// Swap happens after each Godot frame, protected by a mutex.

#include "godot_gl_texture.h"

#include <epoxy/gl.h>
#include <cstdio>
#include <cstring>
#include <pthread.h>

// Double-buffer structure: two FBOs, each with a color texture and depth renderbuffer
struct FBOPair {
  GLuint fbo;
  GLuint color_texture;
  GLuint depth_renderbuffer;
};

struct _GodotGLTexture {
  FlTextureGL parent_instance;

  // Shared GDK GL context
  GdkGLContext* gl_context;

  // Double-buffered FBOs
  FBOPair buffers[2];
  int front_index;  // index into buffers[] that populate() reads from
  int back_index;   // index into buffers[] that Godot renders into

  // Dimensions
  uint32_t width;
  uint32_t height;

  // Thread safety for buffer swap
  pthread_mutex_t swap_mutex;

  // Whether GL resources have been initialized
  gboolean gl_initialized;
};

G_DEFINE_TYPE(GodotGLTexture, godot_gl_texture, fl_texture_gl_get_type())

// ---------------------------------------------------------------------------
// FlTextureGL virtual: populate
// Called by Flutter's raster thread to get the texture to composite.
// We return the FRONT buffer's color texture. Because Flutter's raster
// context and our shared context come from the same GdkWindow, they share
// the GL namespace -- so the texture name is valid in Flutter's context.
// ---------------------------------------------------------------------------
static gboolean godot_gl_texture_populate(FlTextureGL* texture,
                                           uint32_t* target,
                                           uint32_t* name,
                                           uint32_t* width,
                                           uint32_t* height,
                                           GError** error) {
  GodotGLTexture* self = GODOT_GL_TEXTURE(texture);

  if (!self->gl_initialized) {
    g_set_error(error, g_quark_from_string("godot_gl_texture"), 1,
                "GL resources not initialized");
    return FALSE;
  }

  pthread_mutex_lock(&self->swap_mutex);
  int front = self->front_index;
  pthread_mutex_unlock(&self->swap_mutex);

  *target = GL_TEXTURE_2D;
  *name = self->buffers[front].color_texture;
  *width = self->width;
  *height = self->height;

  return TRUE;
}

// ---------------------------------------------------------------------------
// Helper: Create one FBO with color texture + depth renderbuffer
// ---------------------------------------------------------------------------
static gboolean create_fbo(FBOPair* pair, uint32_t width, uint32_t height) {
  // Create color texture
  glGenTextures(1, &pair->color_texture);
  glBindTexture(GL_TEXTURE_2D, pair->color_texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height,
               0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  // Create depth renderbuffer
  glGenRenderbuffers(1, &pair->depth_renderbuffer);
  glBindRenderbuffer(GL_RENDERBUFFER, pair->depth_renderbuffer);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);

  // Create FBO and attach
  glGenFramebuffers(1, &pair->fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, pair->fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                          GL_TEXTURE_2D, pair->color_texture, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                             GL_RENDERBUFFER, pair->depth_renderbuffer);

  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    fprintf(stderr, "[godot_gl_texture] FBO incomplete: 0x%x\n", status);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return FALSE;
  }

  // Clear to dark gray so there's something visible before Godot renders
  glClearColor(0.15f, 0.15f, 0.2f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);

  fprintf(stderr, "[godot_gl_texture] Created FBO %u (color=%u, depth=%u) %ux%u\n",
          pair->fbo, pair->color_texture, pair->depth_renderbuffer,
          width, height);

  return TRUE;
}

// ---------------------------------------------------------------------------
// Helper: Destroy one FBO
// ---------------------------------------------------------------------------
static void destroy_fbo(FBOPair* pair) {
  if (pair->fbo) {
    glDeleteFramebuffers(1, &pair->fbo);
    pair->fbo = 0;
  }
  if (pair->color_texture) {
    glDeleteTextures(1, &pair->color_texture);
    pair->color_texture = 0;
  }
  if (pair->depth_renderbuffer) {
    glDeleteRenderbuffers(1, &pair->depth_renderbuffer);
    pair->depth_renderbuffer = 0;
  }
}

// ---------------------------------------------------------------------------
// GObject dispose
// ---------------------------------------------------------------------------
static void godot_gl_texture_dispose(GObject* object) {
  GodotGLTexture* self = GODOT_GL_TEXTURE(object);

  if (self->gl_initialized && self->gl_context) {
    gdk_gl_context_make_current(self->gl_context);
    destroy_fbo(&self->buffers[0]);
    destroy_fbo(&self->buffers[1]);
    gdk_gl_context_clear_current();
    self->gl_initialized = FALSE;
  }

  g_clear_object(&self->gl_context);
  pthread_mutex_destroy(&self->swap_mutex);

  G_OBJECT_CLASS(godot_gl_texture_parent_class)->dispose(object);
}

// ---------------------------------------------------------------------------
// GObject class/instance init
// ---------------------------------------------------------------------------
static void godot_gl_texture_class_init(GodotGLTextureClass* klass) {
  FL_TEXTURE_GL_CLASS(klass)->populate = godot_gl_texture_populate;
  G_OBJECT_CLASS(klass)->dispose = godot_gl_texture_dispose;
}

static void godot_gl_texture_init(GodotGLTexture* self) {
  self->gl_context = NULL;
  memset(self->buffers, 0, sizeof(self->buffers));
  self->front_index = 0;
  self->back_index = 1;
  self->width = 0;
  self->height = 0;
  self->gl_initialized = FALSE;
  pthread_mutex_init(&self->swap_mutex, NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

GodotGLTexture* godot_gl_texture_new(GdkGLContext* shared_context,
                                      uint32_t width,
                                      uint32_t height) {
  if (!shared_context || width == 0 || height == 0) {
    fprintf(stderr, "[godot_gl_texture] Invalid parameters: ctx=%p w=%u h=%u\n",
            (void*)shared_context, width, height);
    return NULL;
  }

  GodotGLTexture* self = GODOT_GL_TEXTURE(
      g_object_new(GODOT_TYPE_GL_TEXTURE, NULL));
  self->gl_context = GDK_GL_CONTEXT(g_object_ref(shared_context));
  self->width = width;
  self->height = height;

  fprintf(stderr, "[godot_gl_texture] Created %ux%u texture object\n",
          width, height);
  return self;
}

gboolean godot_gl_texture_init_gl(GodotGLTexture* self) {
  if (!self || !self->gl_context) return FALSE;
  if (self->gl_initialized) return TRUE;

  gdk_gl_context_make_current(self->gl_context);

  // Create both FBOs (front and back)
  if (!create_fbo(&self->buffers[0], self->width, self->height)) {
    fprintf(stderr, "[godot_gl_texture] Failed to create front FBO\n");
    gdk_gl_context_clear_current();
    return FALSE;
  }

  if (!create_fbo(&self->buffers[1], self->width, self->height)) {
    fprintf(stderr, "[godot_gl_texture] Failed to create back FBO\n");
    destroy_fbo(&self->buffers[0]);
    gdk_gl_context_clear_current();
    return FALSE;
  }

  glFinish();  // Ensure all GL commands are flushed
  gdk_gl_context_clear_current();

  self->gl_initialized = TRUE;
  fprintf(stderr, "[godot_gl_texture] GL resources initialized (front FBO=%u, back FBO=%u)\n",
          self->buffers[0].fbo, self->buffers[1].fbo);
  return TRUE;
}

uint32_t godot_gl_texture_get_back_texture(GodotGLTexture* self) {
  if (!self || !self->gl_initialized) return 0;
  pthread_mutex_lock(&self->swap_mutex);
  uint32_t tex = self->buffers[self->back_index].color_texture;
  pthread_mutex_unlock(&self->swap_mutex);
  return tex;
}

uint32_t godot_gl_texture_get_back_fbo(GodotGLTexture* self) {
  if (!self || !self->gl_initialized) return 0;
  pthread_mutex_lock(&self->swap_mutex);
  uint32_t fbo = self->buffers[self->back_index].fbo;
  pthread_mutex_unlock(&self->swap_mutex);
  return fbo;
}

void godot_gl_texture_swap_buffers(GodotGLTexture* self) {
  if (!self || !self->gl_initialized) return;

  pthread_mutex_lock(&self->swap_mutex);
  // Swap indices
  int tmp = self->front_index;
  self->front_index = self->back_index;
  self->back_index = tmp;
  pthread_mutex_unlock(&self->swap_mutex);
}

GdkGLContext* godot_gl_texture_get_context(GodotGLTexture* self) {
  return self ? self->gl_context : NULL;
}

uint32_t godot_gl_texture_get_width(GodotGLTexture* self) {
  return self ? self->width : 0;
}

uint32_t godot_gl_texture_get_height(GodotGLTexture* self) {
  return self ? self->height : 0;
}
