// flutter_godot.cc -- Native plugin with webkit2gtk-based WebView
//
// Uses WebKit2GTK (system library) instead of CEF to avoid the 1.4GB
// Chromium download. Renders webkit2gtk WebView in a hidden off-screen
// GtkWindow, captures frames via cairo surface snapshots, and sends pixel
// data to Flutter through FlPixelBufferTexture for inline widget display.
//
// NOTE: A regular GtkWindow (moved off-screen) is used instead of
// GtkOffscreenWindow because WebKit2GTK requires a native X11 window
// for GPU compositing. GtkOffscreenWindow does not create a real X11
// drawable, causing segfaults.

#include "flutter_godot_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>

#ifdef HAS_WEBKIT2GTK

#include <webkit2/webkit2.h>
#include <mutex>

// ─── Pixel Buffer Texture ────────────────────────────────────────────────────

// Custom FlPixelBufferTexture subclass for rendering webview frames
#define WEBVIEW_TEXTURE_TYPE (webview_texture_get_type())
G_DECLARE_FINAL_TYPE(WebviewTexture, webview_texture, WEBVIEW, TEXTURE,
                     FlPixelBufferTexture)

struct _WebviewTexture {
  FlPixelBufferTexture parent_instance;
  uint8_t* buffer;
  int32_t width;
  int32_t height;
  std::mutex* mutex;
};

G_DEFINE_TYPE(WebviewTexture, webview_texture,
              fl_pixel_buffer_texture_get_type())

// Fallback 1x1 opaque-black pixel (RGBA) used when the real buffer is not yet
// ready. Returning FALSE from copy_pixels causes a segfault in Flutter's
// rasterizer (fl_engine_gl_external_texture_frame_callback dereferences null).
static const uint8_t kFallbackPixel[4] = {0, 0, 0, 255};

static gboolean webview_texture_copy_pixels(FlPixelBufferTexture* texture,
                                             const uint8_t** out_buffer,
                                             uint32_t* width,
                                             uint32_t* height,
                                             GError** error) {
  WebviewTexture* self = WEBVIEW_TEXTURE(texture);
  if (self->mutex) {
    self->mutex->lock();
  }
  if (self->buffer != nullptr && self->width > 0 && self->height > 0) {
    *out_buffer = self->buffer;
    *width = static_cast<uint32_t>(self->width);
    *height = static_cast<uint32_t>(self->height);
    if (self->mutex) {
      self->mutex->unlock();
    }
    return TRUE;
  }
  if (self->mutex) {
    self->mutex->unlock();
  }
  // Safety fallback: provide a minimal 1x1 pixel instead of returning FALSE.
  // This prevents a crash in Flutter's rasterizer thread.
  *out_buffer = kFallbackPixel;
  *width = 1;
  *height = 1;
  return TRUE;
}

static void webview_texture_dispose(GObject* object) {
  WebviewTexture* self = WEBVIEW_TEXTURE(object);
  if (self->buffer) {
    delete[] self->buffer;
    self->buffer = nullptr;
  }
  if (self->mutex) {
    delete self->mutex;
    self->mutex = nullptr;
  }
  G_OBJECT_CLASS(webview_texture_parent_class)->dispose(object);
}

static void webview_texture_class_init(WebviewTextureClass* klass) {
  FL_PIXEL_BUFFER_TEXTURE_CLASS(klass)->copy_pixels =
      webview_texture_copy_pixels;
  G_OBJECT_CLASS(klass)->dispose = webview_texture_dispose;
}

static void webview_texture_init(WebviewTexture* self) {
  self->buffer = nullptr;
  self->width = 0;
  self->height = 0;
  self->mutex = new std::mutex();
}

static WebviewTexture* webview_texture_new() {
  return WEBVIEW_TEXTURE(g_object_new(WEBVIEW_TEXTURE_TYPE, nullptr));
}

/// Pre-allocate the pixel buffer with the given dimensions, filled with
/// opaque black. This MUST be called before the Texture widget is rendered
/// in Flutter, because copy_pixels returning FALSE causes a segfault in
/// Flutter's rasterizer thread (fl_engine_gl_external_texture_frame_callback
/// dereferences null when no pixel data is provided).
static void webview_texture_set_size(WebviewTexture* self, int w, int h) {
  if (!self || w <= 0 || h <= 0) return;
  std::lock_guard<std::mutex> lock(*self->mutex);
  delete[] self->buffer;
  int pixel_count = w * h;
  self->buffer = new uint8_t[pixel_count * 4];
  // Fill with opaque black (RGBA: 0,0,0,255)
  for (int i = 0; i < pixel_count; i++) {
    self->buffer[i * 4 + 0] = 0;    // R
    self->buffer[i * 4 + 1] = 0;    // G
    self->buffer[i * 4 + 2] = 0;    // B
    self->buffer[i * 4 + 3] = 255;  // A
  }
  self->width = w;
  self->height = h;
}

// ─── WebView Instance ────────────────────────────────────────────────────────

struct WebViewInstance {
  int64_t texture_id;
  WebviewTexture* texture;
  FlTextureRegistrar* texture_registrar;
  GtkWidget* offscreen_window;
  WebKitWebView* webview;
  guint snapshot_timer;
  int width;
  int height;
  bool disposed;
};

// Global webview instance (single webview per plugin for now)
static WebViewInstance* g_webview_instance = nullptr;

// ─── Snapshot Timer (captures webview frames) ────────────────────────────────

static void on_snapshot_ready(GObject* source_object, GAsyncResult* result,
                               gpointer user_data) {
  WebViewInstance* wv = static_cast<WebViewInstance*>(user_data);
  if (wv->disposed) return;

  GError* error = nullptr;
  cairo_surface_t* surface = webkit_web_view_get_snapshot_finish(
      wv->webview, result, &error);

  if (error) {
    // Snapshot errors are common during page load, don't spam
    g_error_free(error);
    return;
  }

  if (!surface) return;

  // The snapshot surface may not be an image surface (e.g., it could be an
  // Xlib or GL surface depending on the acceleration backend). We need to
  // convert it to an image surface to access raw pixel data.
  cairo_surface_t* image_surface = surface;

  if (cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE) {
    // Convert non-image surface to image surface by painting it
    int cw = cairo_image_surface_get_width(surface);
    int ch = cairo_image_surface_get_height(surface);
    // For non-image surfaces, get dimensions via a temporary context
    if (cw <= 0 || ch <= 0) {
      // Try getting extents via recording surface trick or default to wv size
      cw = wv->width;
      ch = wv->height;
    }
    image_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cw, ch);
    cairo_t* cr = cairo_create(image_surface);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    surface = nullptr;  // surface is consumed, image_surface is now owned
  }

  int sw = cairo_image_surface_get_width(image_surface);
  int sh = cairo_image_surface_get_height(image_surface);

  if (sw <= 0 || sh <= 0) {
    cairo_surface_destroy(image_surface);
    return;
  }

  // Cairo gives us BGRA (CAIRO_FORMAT_ARGB32 is native-endian premultiplied)
  // Flutter expects RGBA
  cairo_surface_flush(image_surface);  // Ensure pixel data is up to date
  unsigned char* cairo_data = cairo_image_surface_get_data(image_surface);
  int stride = cairo_image_surface_get_stride(image_surface);

  if (!cairo_data) {
    // Should not happen for image surfaces, but guard against it
    cairo_surface_destroy(image_surface);
    return;
  }

  int pixel_count = sw * sh;

  uint8_t* rgba_buffer = new uint8_t[pixel_count * 4];

  for (int y = 0; y < sh; y++) {
    unsigned char* row = cairo_data + y * stride;
    for (int x = 0; x < sw; x++) {
      int src_idx = x * 4;
      int dst_idx = (y * sw + x) * 4;
      // BGRA -> RGBA
      rgba_buffer[dst_idx + 0] = row[src_idx + 2]; // R
      rgba_buffer[dst_idx + 1] = row[src_idx + 1]; // G
      rgba_buffer[dst_idx + 2] = row[src_idx + 0]; // B
      rgba_buffer[dst_idx + 3] = row[src_idx + 3]; // A
    }
  }

  cairo_surface_destroy(image_surface);

  // Update the texture buffer
  WebviewTexture* tex = wv->texture;
  if (tex && tex->mutex) {
    tex->mutex->lock();
    delete[] tex->buffer;
    tex->buffer = rgba_buffer;
    tex->width = sw;
    tex->height = sh;
    tex->mutex->unlock();

    // Notify Flutter that a new frame is available
    fl_texture_registrar_mark_texture_frame_available(
        wv->texture_registrar, FL_TEXTURE(tex));
  } else {
    delete[] rgba_buffer;
  }
}

static gboolean snapshot_timer_callback(gpointer user_data) {
  WebViewInstance* wv = static_cast<WebViewInstance*>(user_data);
  if (wv->disposed || !wv->webview) return G_SOURCE_REMOVE;

  webkit_web_view_get_snapshot(
      wv->webview,
      WEBKIT_SNAPSHOT_REGION_FULL_DOCUMENT,
      WEBKIT_SNAPSHOT_OPTIONS_NONE,
      nullptr,  // GCancellable
      on_snapshot_ready,
      wv);

  return G_SOURCE_CONTINUE;
}

static void on_load_changed(WebKitWebView* web_view,
                             WebKitLoadEvent load_event,
                             gpointer user_data) {
  WebViewInstance* wv = static_cast<WebViewInstance*>(user_data);
  switch (load_event) {
    case WEBKIT_LOAD_STARTED:
      fprintf(stderr, "[flutter_godot] WebView: load started\n");
      break;
    case WEBKIT_LOAD_COMMITTED:
      fprintf(stderr, "[flutter_godot] WebView: load committed\n");
      // Start snapshot timer only after page content is committed.
      // Requesting snapshots before any content is loaded can fail.
      if (wv && !wv->disposed && wv->snapshot_timer == 0) {
        fprintf(stderr,
                "[flutter_godot] Starting snapshot capture (~30fps)\n");
        wv->snapshot_timer = g_timeout_add(33, snapshot_timer_callback, wv);
      }
      break;
    case WEBKIT_LOAD_FINISHED:
      fprintf(stderr, "[flutter_godot] WebView: load finished\n");
      break;
    default:
      break;
  }
}

// ─── Cleanup helper ──────────────────────────────────────────────────────────

static void cleanup_webview_instance() {
  if (!g_webview_instance) return;

  g_webview_instance->disposed = true;
  if (g_webview_instance->snapshot_timer) {
    g_source_remove(g_webview_instance->snapshot_timer);
    g_webview_instance->snapshot_timer = 0;
  }
  if (g_webview_instance->texture) {
    fl_texture_registrar_unregister_texture(
        g_webview_instance->texture_registrar,
        FL_TEXTURE(g_webview_instance->texture));
    g_object_unref(g_webview_instance->texture);
    g_webview_instance->texture = nullptr;
  }
  if (g_webview_instance->offscreen_window) {
    gtk_widget_destroy(g_webview_instance->offscreen_window);
    g_webview_instance->offscreen_window = nullptr;
  }
  g_webview_instance->webview = nullptr;  // destroyed with offscreen_window
  delete g_webview_instance;
  g_webview_instance = nullptr;
}

// ─── Method Channel Handlers ─────────────────────────────────────────────────

static void handle_create_webview(FlMethodCall* method_call,
                                   FlTextureRegistrar* texture_registrar) {
  FlValue* args = fl_method_call_get_args(method_call);
  int width = 1280;
  int height = 720;

  if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
    FlValue* w = fl_value_lookup_string(args, "width");
    FlValue* h = fl_value_lookup_string(args, "height");
    if (w && fl_value_get_type(w) == FL_VALUE_TYPE_INT)
      width = fl_value_get_int(w);
    if (h && fl_value_get_type(h) == FL_VALUE_TYPE_INT)
      height = fl_value_get_int(h);
  }

  // Clean up existing webview if any
  cleanup_webview_instance();

  // Create new instance
  g_webview_instance = new WebViewInstance();
  g_webview_instance->disposed = false;
  g_webview_instance->width = width;
  g_webview_instance->height = height;
  g_webview_instance->texture_registrar = texture_registrar;
  g_webview_instance->snapshot_timer = 0;

  // Create texture and pre-allocate pixel buffer.
  // The buffer MUST be initialized before the texture ID is sent to Dart,
  // because Flutter's rasterizer will immediately try to render the Texture
  // widget and call copy_pixels(). If copy_pixels returns FALSE (no data),
  // the engine segfaults in fl_engine_gl_external_texture_frame_callback.
  g_webview_instance->texture = webview_texture_new();
  webview_texture_set_size(g_webview_instance->texture, width, height);
  fl_texture_registrar_register_texture(
      texture_registrar, FL_TEXTURE(g_webview_instance->texture));
  g_webview_instance->texture_id =
      fl_texture_get_id(FL_TEXTURE(g_webview_instance->texture));

  // Create a hidden window to host the WebView.
  // NOTE: We use a regular GtkWindow instead of GtkOffscreenWindow because
  // WebKit2GTK requires a native X11 window for GPU compositing.
  // GtkOffscreenWindow does NOT create a native X11 drawable, causing crashes:
  //   "Gdk-WARNING: drawable is not a native X11 window"
  //   "Gdk-CRITICAL: gdk_window_get_origin: assertion 'GDK_IS_WINDOW' failed"
  //
  // To prevent this window from appearing on screen, we use multiple strategies:
  //   1. gtk_window_set_opacity(0) - makes it fully transparent
  //   2. Move off-screen to large negative coordinates
  //   3. Skip taskbar/pager, no decorations
  //   4. After realization, set X11 override-redirect to bypass WM placement
  g_webview_instance->offscreen_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size(
      GTK_WINDOW(g_webview_instance->offscreen_window), width, height);
  gtk_widget_set_size_request(
      g_webview_instance->offscreen_window, width, height);
  // Make the window fully transparent so it's invisible even if the WM
  // places it on screen (e.g., clamping negative coordinates to 0,0).
  gtk_widget_set_opacity(g_webview_instance->offscreen_window, 0.0);
  // Move the window off-screen so it's not visible
  gtk_window_move(GTK_WINDOW(g_webview_instance->offscreen_window),
                  -width * 2, -height * 2);
  // Prevent the window from appearing in the taskbar
  gtk_window_set_skip_taskbar_hint(
      GTK_WINDOW(g_webview_instance->offscreen_window), TRUE);
  gtk_window_set_skip_pager_hint(
      GTK_WINDOW(g_webview_instance->offscreen_window), TRUE);
  // Disable window decorations
  gtk_window_set_decorated(
      GTK_WINDOW(g_webview_instance->offscreen_window), FALSE);
  // Set it as a utility window type so window managers treat it specially
  gtk_window_set_type_hint(
      GTK_WINDOW(g_webview_instance->offscreen_window),
      GDK_WINDOW_TYPE_HINT_UTILITY);
  // Accept focus is FALSE - we don't want this window to steal focus
  gtk_window_set_accept_focus(
      GTK_WINDOW(g_webview_instance->offscreen_window), FALSE);
  gtk_window_set_focus_on_map(
      GTK_WINDOW(g_webview_instance->offscreen_window), FALSE);

  // Realize the widget so we get an underlying GDK window, then set
  // override-redirect on the X11 window.  Override-redirect tells the
  // X server to bypass the window manager entirely: no placement policy,
  // no decoration, no focus -- the window appears exactly where we put it.
  gtk_widget_realize(g_webview_instance->offscreen_window);
  {
    GdkWindow* gdk_win =
        gtk_widget_get_window(g_webview_instance->offscreen_window);
    if (gdk_win) {
      gdk_window_set_override_redirect(gdk_win, TRUE);
      // After setting override-redirect, move again to ensure position sticks
      gdk_window_move(gdk_win, -width * 2, -height * 2);
    }
  }

  // Create WebView with settings optimized for Godot WASM
  WebKitSettings* settings = webkit_settings_new();
  webkit_settings_set_enable_javascript(settings, TRUE);
  webkit_settings_set_enable_webgl(settings, TRUE);
  webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);
  webkit_settings_set_hardware_acceleration_policy(
      settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS);

  // Allow loading from localhost
  webkit_settings_set_allow_file_access_from_file_urls(settings, TRUE);
  webkit_settings_set_allow_universal_access_from_file_urls(settings, TRUE);

  g_webview_instance->webview = WEBKIT_WEB_VIEW(
      webkit_web_view_new_with_settings(settings));
  g_object_unref(settings);

  gtk_widget_set_size_request(
      GTK_WIDGET(g_webview_instance->webview), width, height);

  // Connect signals
  g_signal_connect(g_webview_instance->webview, "load-changed",
                    G_CALLBACK(on_load_changed), g_webview_instance);

  // Add webview to offscreen window
  gtk_container_add(GTK_CONTAINER(g_webview_instance->offscreen_window),
                     GTK_WIDGET(g_webview_instance->webview));
  gtk_widget_show_all(g_webview_instance->offscreen_window);

  // NOTE: Snapshot timer is started lazily in on_load_changed() when the
  // page load is committed, to avoid requesting snapshots from an empty view.

  fprintf(stderr,
          "[flutter_godot] WebView created (%dx%d), texture_id=%ld\n",
          width, height, g_webview_instance->texture_id);

  // Return texture ID to Dart
  g_autoptr(FlValue) result = fl_value_new_int(g_webview_instance->texture_id);
  FlMethodResponse* response =
      FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  g_autoptr(GError) error = nullptr;
  fl_method_call_respond(method_call, response, &error);
  g_object_unref(response);
}

static void handle_load_url(FlMethodCall* method_call) {
  if (!g_webview_instance || g_webview_instance->disposed ||
      !g_webview_instance->webview) {
    FlMethodResponse* response = FL_METHOD_RESPONSE(
        fl_method_error_response_new(
            "NO_WEBVIEW", "No webview created", nullptr));
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
    return;
  }

  FlValue* args = fl_method_call_get_args(method_call);
  const char* url = nullptr;

  if (fl_value_get_type(args) == FL_VALUE_TYPE_STRING) {
    url = fl_value_get_string(args);
  } else if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
    FlValue* url_val = fl_value_lookup_string(args, "url");
    if (url_val && fl_value_get_type(url_val) == FL_VALUE_TYPE_STRING) {
      url = fl_value_get_string(url_val);
    }
  }

  if (url) {
    fprintf(stderr, "[flutter_godot] Loading URL: %s\n", url);
    webkit_web_view_load_uri(g_webview_instance->webview, url);

    g_autoptr(FlValue) result = fl_value_new_bool(TRUE);
    FlMethodResponse* response =
        FL_METHOD_RESPONSE(fl_method_success_response_new(result));
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
  } else {
    FlMethodResponse* response = FL_METHOD_RESPONSE(
        fl_method_error_response_new(
            "BAD_ARGS", "URL not provided", nullptr));
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
  }
}

static void handle_evaluate_javascript(FlMethodCall* method_call) {
  if (!g_webview_instance || g_webview_instance->disposed ||
      !g_webview_instance->webview) {
    FlMethodResponse* response = FL_METHOD_RESPONSE(
        fl_method_error_response_new(
            "NO_WEBVIEW", "No webview created", nullptr));
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
    return;
  }

  FlValue* args = fl_method_call_get_args(method_call);
  const char* script = nullptr;

  if (fl_value_get_type(args) == FL_VALUE_TYPE_STRING) {
    script = fl_value_get_string(args);
  } else if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
    FlValue* js_val = fl_value_lookup_string(args, "script");
    if (js_val && fl_value_get_type(js_val) == FL_VALUE_TYPE_STRING) {
      script = fl_value_get_string(js_val);
    }
  }

  if (script) {
    webkit_web_view_evaluate_javascript(
        g_webview_instance->webview,
        script,
        -1,       // length (-1 = null-terminated)
        nullptr,  // world_name
        nullptr,  // source_uri
        nullptr,  // GCancellable
        nullptr,  // callback (fire and forget)
        nullptr); // user_data

    g_autoptr(FlValue) result = fl_value_new_bool(TRUE);
    FlMethodResponse* response =
        FL_METHOD_RESPONSE(fl_method_success_response_new(result));
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
  } else {
    FlMethodResponse* response = FL_METHOD_RESPONSE(
        fl_method_error_response_new(
            "BAD_ARGS", "Script not provided", nullptr));
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
  }
}

static void handle_resize_webview(FlMethodCall* method_call) {
  if (!g_webview_instance || g_webview_instance->disposed) {
    g_autoptr(FlValue) result = fl_value_new_bool(FALSE);
    FlMethodResponse* response =
        FL_METHOD_RESPONSE(fl_method_success_response_new(result));
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
    return;
  }

  FlValue* args = fl_method_call_get_args(method_call);
  if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
    FlValue* w = fl_value_lookup_string(args, "width");
    FlValue* h = fl_value_lookup_string(args, "height");
    int width = g_webview_instance->width;
    int height = g_webview_instance->height;
    if (w && fl_value_get_type(w) == FL_VALUE_TYPE_INT)
      width = fl_value_get_int(w);
    if (h && fl_value_get_type(h) == FL_VALUE_TYPE_INT)
      height = fl_value_get_int(h);

    g_webview_instance->width = width;
    g_webview_instance->height = height;
    gtk_widget_set_size_request(
        g_webview_instance->offscreen_window, width, height);
    gtk_widget_set_size_request(
        GTK_WIDGET(g_webview_instance->webview), width, height);
  }

  g_autoptr(FlValue) result = fl_value_new_bool(TRUE);
  FlMethodResponse* response =
      FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  g_autoptr(GError) error = nullptr;
  fl_method_call_respond(method_call, response, &error);
  g_object_unref(response);
}

static void handle_dispose_webview(FlMethodCall* method_call) {
  cleanup_webview_instance();
  fprintf(stderr, "[flutter_godot] WebView disposed\n");

  g_autoptr(FlValue) result = fl_value_new_bool(TRUE);
  FlMethodResponse* response =
      FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  g_autoptr(GError) error = nullptr;
  fl_method_call_respond(method_call, response, &error);
  g_object_unref(response);
}

#endif  // HAS_WEBKIT2GTK

// ─── Unavailable handler (when webkit2gtk is not present) ────────────────────

#ifndef HAS_WEBKIT2GTK

static void handle_unavailable(FlMethodCall* method_call) {
  FlMethodResponse* response = FL_METHOD_RESPONSE(
      fl_method_error_response_new(
          "UNAVAILABLE",
          "webkit2gtk-4.1 not available. Install libwebkit2gtk-4.1-dev",
          nullptr));
  g_autoptr(GError) error = nullptr;
  fl_method_call_respond(method_call, response, &error);
  g_object_unref(response);
}

#endif  // !HAS_WEBKIT2GTK

// ─── Plugin Core ─────────────────────────────────────────────────────────────

struct _FlutterGodotPlugin {
  GObject parent_instance;
  FlMethodChannel* method_channel;
  FlTextureRegistrar* texture_registrar;
};

G_DEFINE_TYPE(FlutterGodotPlugin, flutter_godot_plugin, G_TYPE_OBJECT)

static void method_call_handler(FlMethodChannel* channel,
                                FlMethodCall* method_call,
                                gpointer user_data) {
  FlutterGodotPlugin* plugin = FLUTTER_GODOT_PLUGIN(user_data);
  (void)plugin;  // May be unused when webkit2gtk is not available
  const gchar* method = fl_method_call_get_name(method_call);

  if (strcmp(method, "getPlatformVersion") == 0) {
#ifdef HAS_WEBKIT2GTK
    g_autoptr(FlValue) result =
        fl_value_new_string("Linux (webkit2gtk WebView)");
#else
    g_autoptr(FlValue) result =
        fl_value_new_string("Linux (no WebView - install libwebkit2gtk-4.1-dev)");
#endif
    FlMethodResponse* response =
        FL_METHOD_RESPONSE(fl_method_success_response_new(result));
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
    g_object_unref(response);
    return;
  }

#ifdef HAS_WEBKIT2GTK
  if (strcmp(method, "createWebView") == 0) {
    handle_create_webview(method_call, plugin->texture_registrar);
    return;
  } else if (strcmp(method, "loadUrl") == 0) {
    handle_load_url(method_call);
    return;
  } else if (strcmp(method, "evaluateJavaScript") == 0) {
    handle_evaluate_javascript(method_call);
    return;
  } else if (strcmp(method, "resizeWebView") == 0) {
    handle_resize_webview(method_call);
    return;
  } else if (strcmp(method, "disposeWebView") == 0) {
    handle_dispose_webview(method_call);
    return;
  }
#else
  // All webview methods return UNAVAILABLE when webkit2gtk is not present
  if (strcmp(method, "createWebView") == 0 ||
      strcmp(method, "loadUrl") == 0 ||
      strcmp(method, "evaluateJavaScript") == 0 ||
      strcmp(method, "resizeWebView") == 0 ||
      strcmp(method, "disposeWebView") == 0) {
    handle_unavailable(method_call);
    return;
  }
#endif

  FlMethodResponse* response =
      FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  g_autoptr(GError) error = nullptr;
  fl_method_call_respond(method_call, response, &error);
  g_object_unref(response);
}

static void flutter_godot_plugin_dispose(GObject* object) {
  FlutterGodotPlugin* plugin = FLUTTER_GODOT_PLUGIN(object);
  g_clear_object(&plugin->method_channel);

#ifdef HAS_WEBKIT2GTK
  cleanup_webview_instance();
#endif

  G_OBJECT_CLASS(flutter_godot_plugin_parent_class)->dispose(object);
}

static void flutter_godot_plugin_class_init(FlutterGodotPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = flutter_godot_plugin_dispose;
}

static void flutter_godot_plugin_init(FlutterGodotPlugin* self) {
  self->method_channel = nullptr;
  self->texture_registrar = nullptr;
}

FlutterGodotPlugin* flutter_godot_plugin_new() {
  return FLUTTER_GODOT_PLUGIN(
      g_object_new(flutter_godot_plugin_get_type(), nullptr));
}

void flutter_godot_plugin_register_with_registrar(
    FlPluginRegistrar* registrar) {
  FlutterGodotPlugin* plugin = flutter_godot_plugin_new();

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();

  plugin->method_channel = fl_method_channel_new(
      fl_plugin_registrar_get_messenger(registrar),
      "flutter_godot_method",
      FL_METHOD_CODEC(codec));

  // Get texture registrar for rendering webview frames
  plugin->texture_registrar =
      fl_plugin_registrar_get_texture_registrar(registrar);

  fl_method_channel_set_method_call_handler(
      plugin->method_channel,
      method_call_handler,
      g_object_ref(plugin),
      g_object_unref);

#ifdef HAS_WEBKIT2GTK
  fprintf(stderr, "[flutter_godot] Plugin registered (webkit2gtk mode)\n");
#else
  fprintf(stderr, "[flutter_godot] Plugin registered (no webkit2gtk - "
                  "install libwebkit2gtk-4.1-dev for WebView support)\n");
#endif

  g_object_unref(plugin);
}
