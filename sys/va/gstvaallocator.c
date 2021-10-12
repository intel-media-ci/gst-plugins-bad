/* GStreamer
 * Copyright (C) 2020 Igalia, S.L.
 *     Author: Víctor Jáquez <vjaquez@igalia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstvaallocator.h"

#include <sys/types.h>
#include <unistd.h>
#include <va/va_drmcommon.h>

#include "gstvacaps.h"
#include "gstvavideoformat.h"

#define GST_CAT_DEFAULT gst_va_memory_debug
GST_DEBUG_CATEGORY_STATIC (gst_va_memory_debug);

static void
_init_debug_category (void)
{
#ifndef GST_DISABLE_GST_DEBUG
  static gsize _init = 0;

  if (g_once_init_enter (&_init)) {
    GST_DEBUG_CATEGORY_INIT (gst_va_memory_debug, "vamemory", 0, "VA memory");
    g_once_init_leave (&_init, 1);
  }
#endif
}

static gboolean
_destroy_surfaces (GstVaDisplay * display, VASurfaceID * surfaces,
    gint num_surfaces)
{
  VADisplay dpy = gst_va_display_get_va_dpy (display);
  VAStatus status;

  g_return_val_if_fail (num_surfaces > 0, FALSE);

  gst_va_display_lock (display);
  status = vaDestroySurfaces (dpy, surfaces, num_surfaces);
  gst_va_display_unlock (display);
  if (status != VA_STATUS_SUCCESS) {
    GST_ERROR ("vaDestroySurfaces: %s", vaErrorStr (status));
    return FALSE;
  }

  return TRUE;

}

static gboolean
_create_surfaces (GstVaDisplay * display, guint rt_format, guint fourcc,
    guint width, guint height, gint usage_hint,
    VASurfaceAttribExternalBuffers * ext_buf, VASurfaceID * surfaces,
    guint num_surfaces)
{
  VADisplay dpy = gst_va_display_get_va_dpy (display);
  /* *INDENT-OFF* */
  VASurfaceAttrib attrs[5] = {
    {
      .type = VASurfaceAttribUsageHint,
      .flags = VA_SURFACE_ATTRIB_SETTABLE,
      .value.type = VAGenericValueTypeInteger,
      .value.value.i = usage_hint,
    },
    {
      .type = VASurfaceAttribMemoryType,
      .flags = VA_SURFACE_ATTRIB_SETTABLE,
      .value.type = VAGenericValueTypeInteger,
      .value.value.i = (ext_buf && ext_buf->num_buffers > 0)
                               ? VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME
                               : VA_SURFACE_ATTRIB_MEM_TYPE_VA,
    },
  };
  /* *INDENT-ON* */
  VAStatus status;
  guint num_attrs = 2;

  g_return_val_if_fail (num_surfaces > 0, FALSE);

  if (fourcc > 0) {
    /* *INDENT-OFF* */
    attrs[num_attrs++] = (VASurfaceAttrib) {
      .type = VASurfaceAttribPixelFormat,
      .flags = VA_SURFACE_ATTRIB_SETTABLE,
      .value.type = VAGenericValueTypeInteger,
      .value.value.i = fourcc,
    };
    /* *INDENT-ON* */
  }

  if (ext_buf) {
    /* *INDENT-OFF* */
    attrs[num_attrs++] = (VASurfaceAttrib) {
      .type = VASurfaceAttribExternalBufferDescriptor,
      .flags = VA_SURFACE_ATTRIB_SETTABLE,
      .value.type = VAGenericValueTypePointer,
      .value.value.p = ext_buf,
    };
    /* *INDENT-ON* */
  }

  gst_va_display_lock (display);
  status = vaCreateSurfaces (dpy, rt_format, width, height, surfaces,
      num_surfaces, attrs, num_attrs);
  gst_va_display_unlock (display);
  if (status != VA_STATUS_SUCCESS) {
    GST_ERROR ("vaCreateSurfaces: %s", vaErrorStr (status));
    return FALSE;
  }

  return TRUE;
}

static gboolean
_export_surface_to_dmabuf (GstVaDisplay * display, VASurfaceID surface,
    guint32 flags, VADRMPRIMESurfaceDescriptor * desc)
{
  VADisplay dpy = gst_va_display_get_va_dpy (display);
  VAStatus status;

  gst_va_display_lock (display);
  status = vaExportSurfaceHandle (dpy, surface,
      VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, flags, desc);
  gst_va_display_unlock (display);
  if (status != VA_STATUS_SUCCESS) {
    GST_ERROR ("vaExportSurfaceHandle: %s", vaErrorStr (status));
    return FALSE;
  }

  return TRUE;
}

static gboolean
_destroy_image (GstVaDisplay * display, VAImageID image_id)
{
  VADisplay dpy = gst_va_display_get_va_dpy (display);
  VAStatus status;

  gst_va_display_lock (display);
  status = vaDestroyImage (dpy, image_id);
  gst_va_display_unlock (display);
  if (status != VA_STATUS_SUCCESS) {
    GST_ERROR ("vaDestroyImage: %s", vaErrorStr (status));
    return FALSE;
  }
  return TRUE;
}

static gboolean
_get_derive_image (GstVaDisplay * display, VASurfaceID surface, VAImage * image)
{
  VADisplay dpy = gst_va_display_get_va_dpy (display);
  VAStatus status;

  gst_va_display_lock (display);
  status = vaDeriveImage (dpy, surface, image);
  gst_va_display_unlock (display);
  if (status != VA_STATUS_SUCCESS) {
    GST_WARNING ("vaDeriveImage: %s", vaErrorStr (status));
    return FALSE;
  }

  return TRUE;
}

static gboolean
_create_image (GstVaDisplay * display, GstVideoFormat format, gint width,
    gint height, VAImage * image)
{
  VADisplay dpy = gst_va_display_get_va_dpy (display);
  const VAImageFormat *va_format;
  VAStatus status;

  va_format = gst_va_image_format_from_video_format (format);
  if (!va_format)
    return FALSE;

  gst_va_display_lock (display);
  status =
      vaCreateImage (dpy, (VAImageFormat *) va_format, width, height, image);
  gst_va_display_unlock (display);
  if (status != VA_STATUS_SUCCESS) {
    GST_ERROR ("vaCreateImage: %s", vaErrorStr (status));
    return FALSE;
  }
  return TRUE;
}

static gboolean
_get_image (GstVaDisplay * display, VASurfaceID surface, VAImage * image)
{
  VADisplay dpy = gst_va_display_get_va_dpy (display);
  VAStatus status;

  gst_va_display_lock (display);
  status = vaGetImage (dpy, surface, 0, 0, image->width, image->height,
      image->image_id);
  gst_va_display_unlock (display);
  if (status != VA_STATUS_SUCCESS) {
    GST_ERROR ("vaGetImage: %s", vaErrorStr (status));
    return FALSE;
  }

  return TRUE;
}

static gboolean
_sync_surface (GstVaDisplay * display, VASurfaceID surface)
{
  VADisplay dpy = gst_va_display_get_va_dpy (display);
  VAStatus status;

  gst_va_display_lock (display);
  status = vaSyncSurface (dpy, surface);
  gst_va_display_unlock (display);
  if (status != VA_STATUS_SUCCESS) {
    GST_WARNING ("vaSyncSurface: %s", vaErrorStr (status));
    return FALSE;
  }
  return TRUE;
}

static gboolean
_map_buffer (GstVaDisplay * display, VABufferID buffer, gpointer * data)
{
  VADisplay dpy = gst_va_display_get_va_dpy (display);
  VAStatus status;

  gst_va_display_lock (display);
  status = vaMapBuffer (dpy, buffer, data);
  gst_va_display_unlock (display);
  if (status != VA_STATUS_SUCCESS) {
    GST_WARNING ("vaMapBuffer: %s", vaErrorStr (status));
    return FALSE;
  }
  return TRUE;
}

static gboolean
_unmap_buffer (GstVaDisplay * display, VABufferID buffer)
{
  VADisplay dpy = gst_va_display_get_va_dpy (display);
  VAStatus status;

  gst_va_display_lock (display);
  status = vaUnmapBuffer (dpy, buffer);
  gst_va_display_unlock (display);
  if (status != VA_STATUS_SUCCESS) {
    GST_WARNING ("vaUnmapBuffer: %s", vaErrorStr (status));
    return FALSE;
  }
  return TRUE;
}

static gboolean
_put_image (GstVaDisplay * display, VASurfaceID surface, VAImage * image)
{
  VADisplay dpy = gst_va_display_get_va_dpy (display);
  VAStatus status;

  if (!_sync_surface (display, surface))
    return FALSE;

  gst_va_display_lock (display);
  status = vaPutImage (dpy, surface, image->image_id, 0, 0, image->width,
      image->height, 0, 0, image->width, image->height);
  gst_va_display_unlock (display);
  if (status != VA_STATUS_SUCCESS) {
    GST_ERROR ("vaPutImage: %s", vaErrorStr (status));
    return FALSE;
  }
  return TRUE;
}

/*=========================== Quarks for GstMemory ===========================*/

static GQuark
gst_va_buffer_surface_quark (void)
{
  static gsize surface_quark = 0;

  if (g_once_init_enter (&surface_quark)) {
    GQuark quark = g_quark_from_string ("GstVaBufferSurface");
    g_once_init_leave (&surface_quark, quark);
  }

  return surface_quark;
}

static GQuark
gst_va_drm_mod_quark (void)
{
  static gsize drm_mod_quark = 0;

  if (g_once_init_enter (&drm_mod_quark)) {
    GQuark quark = g_quark_from_string ("DRMModifier");
    g_once_init_leave (&drm_mod_quark, quark);
  }

  return drm_mod_quark;
}

static GQuark
gst_va_buffer_aux_surface_quark (void)
{
  static gsize surface_quark = 0;

  if (g_once_init_enter (&surface_quark)) {
    GQuark quark = g_quark_from_string ("GstVaBufferAuxSurface");
    g_once_init_leave (&surface_quark, quark);
  }

  return surface_quark;
}

/*========================= GstVaBufferSurface ===============================*/

typedef struct _GstVaBufferSurface GstVaBufferSurface;
struct _GstVaBufferSurface
{
  GstVaDisplay *display;
  VASurfaceID surface;
  guint n_mems;
  GstMemory *mems[GST_VIDEO_MAX_PLANES];
  gint ref_count;
  gint ref_mems_count;
};

static void
gst_va_buffer_surface_unref (gpointer data)
{
  GstVaBufferSurface *buf = data;

  g_return_if_fail (buf && GST_IS_VA_DISPLAY (buf->display));

  if (g_atomic_int_dec_and_test (&buf->ref_count)) {
    GST_LOG_OBJECT (buf->display, "Destroying surface %#x", buf->surface);
    _destroy_surfaces (buf->display, &buf->surface, 1);
    gst_clear_object (&buf->display);
    g_slice_free (GstVaBufferSurface, buf);
  }
}

static GstVaBufferSurface *
gst_va_buffer_surface_new (VASurfaceID surface, GstVideoFormat format,
    gint width, gint height)
{
  GstVaBufferSurface *buf = g_slice_new (GstVaBufferSurface);

  g_atomic_int_set (&buf->ref_count, 0);
  g_atomic_int_set (&buf->ref_mems_count, 0);
  buf->surface = surface;
  buf->display = NULL;
  buf->n_mems = 0;

  return buf;
}

/*=========================== GstVaMemoryPool ================================*/

/* queue for disposed surfaces */
typedef struct _GstVaMemoryPool GstVaMemoryPool;
struct _GstVaMemoryPool
{
  GstAtomicQueue *queue;
  gint surface_count;

  GMutex lock;
};

#define GST_VA_MEMORY_POOL_CAST(obj) ((GstVaMemoryPool *)obj)
#define GST_VA_MEMORY_POOL_LOCK(obj) g_mutex_lock (&GST_VA_MEMORY_POOL_CAST(obj)->lock)
#define GST_VA_MEMORY_POOL_UNLOCK(obj) g_mutex_unlock (&GST_VA_MEMORY_POOL_CAST(obj)->lock)

static void
gst_va_memory_pool_init (GstVaMemoryPool * self)
{
  self->queue = gst_atomic_queue_new (2);

  g_mutex_init (&self->lock);

  self->surface_count = 0;
}

static void
gst_va_memory_pool_finalize (GstVaMemoryPool * self)
{
  g_mutex_clear (&self->lock);

  gst_atomic_queue_unref (self->queue);
}

static void
gst_va_memory_pool_flush_unlocked (GstVaMemoryPool * self,
    GstVaDisplay * display)
{
  GstMemory *mem;
  GstVaBufferSurface *buf;

  while ((mem = gst_atomic_queue_pop (self->queue))) {
    /* destroy the surface */
    buf = gst_mini_object_get_qdata (GST_MINI_OBJECT (mem),
        gst_va_buffer_surface_quark ());
    if (buf) {
      if (g_atomic_int_dec_and_test (&buf->ref_count)) {
        GST_LOG ("Destroying surface %#x", buf->surface);
        _destroy_surfaces (display, &buf->surface, 1);
        self->surface_count -= 1;       /* GstVaDmabufAllocator */
        g_slice_free (GstVaBufferSurface, buf);
      }
    } else {
      self->surface_count -= 1; /* GstVaAllocator */
    }

    GST_MINI_OBJECT_CAST (mem)->dispose = NULL;
    /* when mem are pushed available queue its allocator is unref,
     * then now it is required to ref the allocator here because
     * memory's finalize will unref it again */
    gst_object_ref (mem->allocator);
    gst_memory_unref (mem);
  }
}

static void
gst_va_memory_pool_flush (GstVaMemoryPool * self, GstVaDisplay * display)
{
  GST_VA_MEMORY_POOL_LOCK (self);
  gst_va_memory_pool_flush_unlocked (self, display);
  GST_VA_MEMORY_POOL_UNLOCK (self);
}

static inline void
gst_va_memory_pool_push (GstVaMemoryPool * self, GstMemory * mem)
{
  gst_atomic_queue_push (self->queue, gst_memory_ref (mem));
}

static inline GstMemory *
gst_va_memory_pool_pop (GstVaMemoryPool * self)
{
  return gst_atomic_queue_pop (self->queue);
}

static inline GstMemory *
gst_va_memory_pool_peek (GstVaMemoryPool * self)
{
  return gst_atomic_queue_peek (self->queue);
}

static inline guint
gst_va_memory_pool_surface_count (GstVaMemoryPool * self)
{
  return g_atomic_int_get (&self->surface_count);
}

static inline void
gst_va_memory_pool_surface_inc (GstVaMemoryPool * self)
{
  g_atomic_int_inc (&self->surface_count);
}

/*=========================== GstVaDmabufAllocator ===========================*/

struct _GstVaDmabufAllocator
{
  GstDmaBufAllocator parent;

  GstVaDisplay *display;

  GstMemoryMapFunction parent_map;

  GstVideoInfo info;
  guint usage_hint;

  GstVaMemoryPool pool;
};

#define gst_va_dmabuf_allocator_parent_class dmabuf_parent_class
G_DEFINE_TYPE_WITH_CODE (GstVaDmabufAllocator, gst_va_dmabuf_allocator,
    GST_TYPE_DMABUF_ALLOCATOR, _init_debug_category ());

static gpointer
gst_va_dmabuf_mem_map (GstMemory * gmem, gsize maxsize, GstMapFlags flags)
{
  GstVaDmabufAllocator *self = GST_VA_DMABUF_ALLOCATOR (gmem->allocator);
  VASurfaceID surface = gst_va_memory_get_surface (gmem);
  guint64 *drm_mod;

  drm_mod = gst_mini_object_get_qdata (GST_MINI_OBJECT (gmem),
      gst_va_drm_mod_quark ());

  /* 0 is DRM_FORMAT_MOD_LINEAR, we do not include its header now. */
  if (*drm_mod != 0) {
    GST_ERROR_OBJECT (self, "Failed to map the dmabuf because the modifier "
        "is: %#lx, which is not linear.", *drm_mod);
    return NULL;
  }

  _sync_surface (self->display, surface);

  return self->parent_map (gmem, maxsize, flags);
}

static void
gst_va_dmabuf_allocator_finalize (GObject * object)
{
  GstVaDmabufAllocator *self = GST_VA_DMABUF_ALLOCATOR (object);

  gst_va_memory_pool_finalize (&self->pool);
  gst_clear_object (&self->display);

  G_OBJECT_CLASS (dmabuf_parent_class)->finalize (object);
}

static void
gst_va_dmabuf_allocator_dispose (GObject * object)
{
  GstVaDmabufAllocator *self = GST_VA_DMABUF_ALLOCATOR (object);

  gst_va_memory_pool_flush_unlocked (&self->pool, self->display);
  if (gst_va_memory_pool_surface_count (&self->pool) != 0) {
    GST_WARNING_OBJECT (self, "Surfaces leaked: %d",
        gst_va_memory_pool_surface_count (&self->pool));
  }

  G_OBJECT_CLASS (dmabuf_parent_class)->dispose (object);
}

static void
gst_va_dmabuf_allocator_class_init (GstVaDmabufAllocatorClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gst_va_dmabuf_allocator_dispose;
  object_class->finalize = gst_va_dmabuf_allocator_finalize;
}

static void
gst_va_dmabuf_allocator_init (GstVaDmabufAllocator * self)
{
  gst_va_memory_pool_init (&self->pool);
  self->parent_map = GST_ALLOCATOR (self)->mem_map;
  GST_ALLOCATOR (self)->mem_map = gst_va_dmabuf_mem_map;
}

GstAllocator *
gst_va_dmabuf_allocator_new (GstVaDisplay * display)
{
  GstVaDmabufAllocator *self;

  g_return_val_if_fail (GST_IS_VA_DISPLAY (display), NULL);

  self = g_object_new (GST_TYPE_VA_DMABUF_ALLOCATOR, NULL);
  self->display = gst_object_ref (display);
  gst_object_ref_sink (self);

  return GST_ALLOCATOR (self);
}

static inline goffset
_get_fd_size (gint fd)
{
  return lseek (fd, 0, SEEK_END);
}

static gboolean
gst_va_dmabuf_memory_release (GstMiniObject * mini_object)
{
  GstMemory *mem = GST_MEMORY_CAST (mini_object);
  GstVaBufferSurface *buf;
  GstVaDmabufAllocator *self = GST_VA_DMABUF_ALLOCATOR (mem->allocator);
  guint i;

  buf = gst_mini_object_get_qdata (GST_MINI_OBJECT (mem),
      gst_va_buffer_surface_quark ());
  if (!buf)
    return TRUE;                /* free this unknown buffer */

  /* if this is the last reference to the GstVaBufferSurface, iterates
   * its array of memories to push them into the queue with thread
   * safetly. */
  GST_VA_MEMORY_POOL_LOCK (&self->pool);
  if (g_atomic_int_dec_and_test (&buf->ref_mems_count)) {
    for (i = 0; i < buf->n_mems; i++) {
      GST_LOG_OBJECT (self, "releasing %p: dmabuf %d, va surface %#x",
          buf->mems[i], gst_dmabuf_memory_get_fd (buf->mems[i]), buf->surface);
      gst_va_memory_pool_push (&self->pool, buf->mems[i]);
    }
  }
  GST_VA_MEMORY_POOL_UNLOCK (&self->pool);

  /* note: if ref_mem_count doesn't reach zero, that memory will
   * "float" until it's pushed back into the pool by the last va
   * buffer surface ref */

  /* Keep last in case we are holding on the last allocator ref */
  gst_object_unref (mem->allocator);

  /* don't call mini_object's free */
  return FALSE;
}

/* Creates an exported VASurface and adds it as @buffer's memories
 * qdata
 *
 * If @info is not NULL, a dummy (non-pooled) buffer is created to
 * update offsets and strides, and it has to be unrefed immediately.
 */
static gboolean
gst_va_dmabuf_allocator_setup_buffer_full (GstAllocator * allocator,
    GstBuffer * buffer, GstVideoInfo * info)
{
  GstVaBufferSurface *buf;
  GstVaDmabufAllocator *self = GST_VA_DMABUF_ALLOCATOR (allocator);
  GstVideoFormat format;
  VADRMPRIMESurfaceDescriptor desc = { 0, };
  VASurfaceAttribExternalBuffers *extbuf = NULL, ext_buf;
  VASurfaceID surface;
  guint32 i, fourcc, rt_format, export_flags;
  GDestroyNotify buffer_destroy = NULL;

  g_return_val_if_fail (GST_IS_VA_DMABUF_ALLOCATOR (allocator), FALSE);

  format = GST_VIDEO_INFO_FORMAT (&self->info);
  fourcc = gst_va_fourcc_from_video_format (format);
  rt_format = gst_va_chroma_from_video_format (format);
  if (fourcc == 0 || rt_format == 0) {
    GST_ERROR_OBJECT (allocator, "Unsupported format: %s",
        gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (&self->info)));
    return FALSE;
  }

  /* HACK(victor): disable tiling for i965 driver for RGB formats */
  if (gst_va_display_is_implementation (self->display,
          GST_VA_IMPLEMENTATION_INTEL_I965)
      && GST_VIDEO_INFO_IS_RGB (&self->info)) {
    /* *INDENT-OFF* */
    ext_buf = (VASurfaceAttribExternalBuffers) {
      .width = GST_VIDEO_INFO_WIDTH (&self->info),
      .height = GST_VIDEO_INFO_HEIGHT (&self->info),
      .num_planes = GST_VIDEO_INFO_N_PLANES (&self->info),
      .pixel_format = fourcc,
    };
    /* *INDENT-ON* */

    extbuf = &ext_buf;
  }

  if (!_create_surfaces (self->display, rt_format, fourcc,
          GST_VIDEO_INFO_WIDTH (&self->info),
          GST_VIDEO_INFO_HEIGHT (&self->info), self->usage_hint, extbuf,
          &surface, 1))
    return FALSE;

  /* workaround for missing layered dmabuf formats in i965 */
  if (gst_va_display_is_implementation (self->display,
          GST_VA_IMPLEMENTATION_INTEL_I965)
      && (fourcc == VA_FOURCC_YUY2 || fourcc == VA_FOURCC_UYVY)) {
    /* These are not representable as separate planes */
    export_flags = VA_EXPORT_SURFACE_COMPOSED_LAYERS;
  } else {
    /* Each layer will contain exactly one plane.  For example, an NV12
     * surface will be exported as two layers */
    export_flags = VA_EXPORT_SURFACE_SEPARATE_LAYERS;
  }

  export_flags |= VA_EXPORT_SURFACE_READ_WRITE;

  if (!_export_surface_to_dmabuf (self->display, surface, export_flags, &desc))
    goto failed;

  g_assert (GST_VIDEO_INFO_N_PLANES (&self->info) == desc.num_layers);

  if (fourcc != desc.fourcc) {
    GST_ERROR ("Unsupported fourcc: %" GST_FOURCC_FORMAT,
        GST_FOURCC_ARGS (desc.fourcc));
    goto failed;
  }

  buf = gst_va_buffer_surface_new (surface, format, desc.width, desc.height);
  if (G_UNLIKELY (info)) {
    *info = self->info;
    GST_VIDEO_INFO_SIZE (info) = 0;
  }

  buf->n_mems = desc.num_objects;

  for (i = 0; i < desc.num_objects; i++) {
    gint fd = desc.objects[i].fd;
    gsize size = desc.objects[i].size > 0 ?
        desc.objects[i].size : _get_fd_size (fd);
    GstMemory *mem = gst_dmabuf_allocator_alloc (allocator, fd, size);
    guint64 *drm_mod = g_new (guint64, 1);

    gst_buffer_append_memory (buffer, mem);
    buf->mems[i] = mem;

    if (G_LIKELY (!info)) {
      GST_MINI_OBJECT (mem)->dispose = gst_va_dmabuf_memory_release;
      g_atomic_int_add (&buf->ref_mems_count, 1);
    } else {
      /* if no @info, surface will be destroyed as soon as buffer is
       * destroyed (e.g. gst_va_dmabuf_allocator_try()) */
      buf->display = gst_object_ref (self->display);
      buffer_destroy = gst_va_buffer_surface_unref;
    }

    g_atomic_int_add (&buf->ref_count, 1);
    gst_mini_object_set_qdata (GST_MINI_OBJECT (mem),
        gst_va_buffer_surface_quark (), buf, buffer_destroy);

    *drm_mod = desc.objects[i].drm_format_modifier;
    gst_mini_object_set_qdata (GST_MINI_OBJECT (mem), gst_va_drm_mod_quark (),
        drm_mod, g_free);

    if (G_UNLIKELY (info))
      GST_VIDEO_INFO_SIZE (info) += size;

    GST_LOG_OBJECT (self, "buffer %p: new dmabuf %d / surface %#x [%dx%d] "
        "size %" G_GSIZE_FORMAT, buffer, fd, surface,
        GST_VIDEO_INFO_WIDTH (&self->info), GST_VIDEO_INFO_HEIGHT (&self->info),
        GST_VIDEO_INFO_SIZE (&self->info));
  }

  if (!desc.num_objects)
    gst_va_buffer_surface_unref (buf);

  if (G_UNLIKELY (info)) {
    for (i = 0; i < desc.num_layers; i++) {
      g_assert (desc.layers[i].num_planes == 1);
      GST_VIDEO_INFO_PLANE_OFFSET (info, i) = desc.layers[i].offset[0];
      GST_VIDEO_INFO_PLANE_STRIDE (info, i) = desc.layers[i].pitch[0];
    }
  } else {
    gst_va_memory_pool_surface_inc (&self->pool);
  }

  return TRUE;

failed:
  {
    _destroy_surfaces (self->display, &surface, 1);
    return FALSE;
  }
}

gboolean
gst_va_dmabuf_allocator_setup_buffer (GstAllocator * allocator,
    GstBuffer * buffer)
{
  return gst_va_dmabuf_allocator_setup_buffer_full (allocator, buffer, NULL);
}

static VASurfaceID
gst_va_dmabuf_allocator_prepare_buffer_unlocked (GstVaDmabufAllocator * self,
    GstBuffer * buffer)
{
  GstMemory *mems[GST_VIDEO_MAX_PLANES] = { 0, };
  GstVaBufferSurface *buf;
  gint i, j, idx;

  mems[0] = gst_va_memory_pool_pop (&self->pool);
  if (!mems[0])
    return VA_INVALID_ID;

  buf = gst_mini_object_get_qdata (GST_MINI_OBJECT (mems[0]),
      gst_va_buffer_surface_quark ());
  if (!buf)
    return VA_INVALID_ID;

  if (buf->surface == VA_INVALID_ID)
    return VA_INVALID_ID;

  for (idx = 1; idx < buf->n_mems; idx++) {
    /* grab next memory from queue */
    {
      GstMemory *mem;
      GstVaBufferSurface *pbuf;

      mem = gst_va_memory_pool_peek (&self->pool);
      if (!mem)
        return VA_INVALID_ID;

      pbuf = gst_mini_object_get_qdata (GST_MINI_OBJECT (mem),
          gst_va_buffer_surface_quark ());
      if (!pbuf)
        return VA_INVALID_ID;

      if (pbuf->surface != buf->surface) {
        GST_WARNING_OBJECT (self,
            "expecting memory with surface %#x but got %#x: "
            "possible memory interweaving", buf->surface, pbuf->surface);
        return VA_INVALID_ID;
      }
    }

    mems[idx] = gst_va_memory_pool_pop (&self->pool);
  };

  /* append memories */
  for (i = 0; i < buf->n_mems; i++) {
    gboolean found = FALSE;

    /* find next memory to append */
    for (j = 0; j < idx; j++) {
      if (buf->mems[i] == mems[j]) {
        found = TRUE;
        break;
      }
    }

    /* if not found, free all the popped memories and bail */
    if (!found) {
      if (!buf->display)
        buf->display = gst_object_ref (self->display);
      for (j = 0; j < idx; j++) {
        gst_object_ref (buf->mems[j]->allocator);
        GST_MINI_OBJECT (mems[j])->dispose = NULL;
        gst_memory_unref (mems[j]);
      }
      return VA_INVALID_ID;
    }

    g_atomic_int_add (&buf->ref_mems_count, 1);
    gst_object_ref (buf->mems[i]->allocator);
    gst_buffer_append_memory (buffer, buf->mems[i]);

    GST_LOG ("bufer %p: memory %p - dmabuf %d / surface %#x", buffer,
        buf->mems[i], gst_dmabuf_memory_get_fd (buf->mems[i]),
        gst_va_memory_get_surface (buf->mems[i]));
  }

  return buf->surface;
}

gboolean
gst_va_dmabuf_allocator_prepare_buffer (GstAllocator * allocator,
    GstBuffer * buffer)
{
  GstVaDmabufAllocator *self = GST_VA_DMABUF_ALLOCATOR (allocator);
  VASurfaceID surface;

  GST_VA_MEMORY_POOL_LOCK (&self->pool);
  surface = gst_va_dmabuf_allocator_prepare_buffer_unlocked (self, buffer);
  GST_VA_MEMORY_POOL_UNLOCK (&self->pool);

  return (surface != VA_INVALID_ID);
}

void
gst_va_dmabuf_allocator_flush (GstAllocator * allocator)
{
  GstVaDmabufAllocator *self = GST_VA_DMABUF_ALLOCATOR (allocator);

  gst_va_memory_pool_flush (&self->pool, self->display);
}

static gboolean
gst_va_dmabuf_allocator_try (GstAllocator * allocator)
{
  GstBuffer *buffer;
  GstVaDmabufAllocator *self = GST_VA_DMABUF_ALLOCATOR (allocator);
  GstVideoInfo info = self->info;
  gboolean ret;

  buffer = gst_buffer_new ();
  ret = gst_va_dmabuf_allocator_setup_buffer_full (allocator, buffer, &info);
  gst_buffer_unref (buffer);

  if (ret)
    self->info = info;

  return ret;
}

gboolean
gst_va_dmabuf_allocator_set_format (GstAllocator * allocator,
    GstVideoInfo * info, guint usage_hint)
{
  GstVaDmabufAllocator *self;
  gboolean ret;

  g_return_val_if_fail (GST_IS_VA_DMABUF_ALLOCATOR (allocator), FALSE);
  g_return_val_if_fail (info, FALSE);

  self = GST_VA_DMABUF_ALLOCATOR (allocator);

  if (gst_va_memory_pool_surface_count (&self->pool) != 0) {
    if (GST_VIDEO_INFO_FORMAT (info) == GST_VIDEO_INFO_FORMAT (&self->info)
        && GST_VIDEO_INFO_WIDTH (info) == GST_VIDEO_INFO_WIDTH (&self->info)
        && GST_VIDEO_INFO_HEIGHT (info) == GST_VIDEO_INFO_HEIGHT (&self->info)
        && usage_hint == self->usage_hint) {
      *info = self->info;       /* update callee info (offset & stride) */
      return TRUE;
    }
    return FALSE;
  }

  self->usage_hint = usage_hint;
  self->info = *info;

  ret = gst_va_dmabuf_allocator_try (allocator);

  if (ret)
    *info = self->info;

  return ret;
}

gboolean
gst_va_dmabuf_allocator_get_format (GstAllocator * allocator,
    GstVideoInfo * info, guint * usage_hint)
{
  GstVaDmabufAllocator *self = GST_VA_DMABUF_ALLOCATOR (allocator);

  if (GST_VIDEO_INFO_FORMAT (&self->info) == GST_VIDEO_FORMAT_UNKNOWN)
    return FALSE;

  if (info)
    *info = self->info;
  if (usage_hint)
    *usage_hint = self->usage_hint;

  return TRUE;
}

/* XXX: use a surface pool to control the created surfaces */
gboolean
gst_va_dmabuf_memories_setup (GstVaDisplay * display, GstVideoInfo * info,
    guint n_planes, GstMemory * mem[GST_VIDEO_MAX_PLANES],
    uintptr_t * fds, gsize offset[GST_VIDEO_MAX_PLANES], guint usage_hint)
{
  GstVideoFormat format;
  GstVaBufferSurface *buf;
  /* *INDENT-OFF* */
  VASurfaceAttribExternalBuffers ext_buf = {
    .width = GST_VIDEO_INFO_WIDTH (info),
    .height = GST_VIDEO_INFO_HEIGHT (info),
    .data_size = GST_VIDEO_INFO_SIZE (info),
    .num_planes = GST_VIDEO_INFO_N_PLANES (info),
    .buffers = fds,
    .num_buffers = GST_VIDEO_INFO_N_PLANES (info),
  };
  /* *INDENT-ON* */
  VASurfaceID surface;
  guint32 fourcc, rt_format;
  guint i;
  gboolean ret;

  g_return_val_if_fail (GST_IS_VA_DISPLAY (display), FALSE);
  g_return_val_if_fail (n_planes <= GST_VIDEO_MAX_PLANES, FALSE);

  format = GST_VIDEO_INFO_FORMAT (info);
  if (format == GST_VIDEO_FORMAT_UNKNOWN)
    return FALSE;

  rt_format = gst_va_chroma_from_video_format (format);
  if (rt_format == 0)
    return FALSE;

  fourcc = gst_va_fourcc_from_video_format (format);
  if (fourcc == 0)
    return FALSE;

  ext_buf.pixel_format = fourcc;

  for (i = 0; i < n_planes; i++) {
    ext_buf.pitches[i] = GST_VIDEO_INFO_PLANE_STRIDE (info, i);
    ext_buf.offsets[i] = offset[i];
  }

  ret = _create_surfaces (display, rt_format, ext_buf.pixel_format,
      ext_buf.width, ext_buf.height, usage_hint, &ext_buf, &surface, 1);
  if (!ret)
    return FALSE;

  GST_LOG_OBJECT (display, "Created surface %#x [%dx%d]", surface,
      ext_buf.width, ext_buf.height);

  buf = gst_va_buffer_surface_new (surface, rt_format, ext_buf.width,
      ext_buf.height);
  buf->display = gst_object_ref (display);
  buf->n_mems = n_planes;
  memcpy (buf->mems, mem, sizeof (buf->mems));

  for (i = 0; i < n_planes; i++) {
    g_atomic_int_add (&buf->ref_count, 1);
    gst_mini_object_set_qdata (GST_MINI_OBJECT (mem[i]),
        gst_va_buffer_surface_quark (), buf, gst_va_buffer_surface_unref);
    GST_INFO_OBJECT (display, "setting surface %#x to dmabuf fd %d",
        buf->surface, gst_dmabuf_memory_get_fd (mem[i]));
  }

  if (!n_planes)
    gst_va_buffer_surface_unref (buf);

  return TRUE;
}

/*===================== GstVaAllocator / GstVaMemory =========================*/

struct _GstVaAllocator
{
  GstAllocator parent;

  GstVaDisplay *display;

  gboolean use_derived;
  GArray *surface_formats;

  GstVideoFormat surface_format;
  GstVideoFormat img_format;
  guint32 fourcc;
  guint32 rt_format;

  GstVideoInfo derived_info;
  GstVideoInfo info;
  guint usage_hint;

  GstVaMemoryPool pool;
};

typedef struct _GstVaMemory GstVaMemory;
struct _GstVaMemory
{
  GstMemory mem;

  VASurfaceID surface;
  GstVideoFormat surface_format;
  VAImage image;
  gpointer mapped_data;

  GstMapFlags prev_mapflags;
  gint map_count;

  gboolean is_derived;
  gboolean is_dirty;
  GMutex lock;
};

G_DEFINE_TYPE_WITH_CODE (GstVaAllocator, gst_va_allocator, GST_TYPE_ALLOCATOR,
    _init_debug_category ());

static gboolean _va_unmap (GstVaMemory * mem);

static void
gst_va_allocator_finalize (GObject * object)
{
  GstVaAllocator *self = GST_VA_ALLOCATOR (object);

  gst_va_memory_pool_finalize (&self->pool);
  g_clear_pointer (&self->surface_formats, g_array_unref);
  gst_clear_object (&self->display);

  G_OBJECT_CLASS (gst_va_allocator_parent_class)->finalize (object);
}

static void
gst_va_allocator_dispose (GObject * object)
{
  GstVaAllocator *self = GST_VA_ALLOCATOR (object);

  gst_va_memory_pool_flush_unlocked (&self->pool, self->display);
  if (gst_va_memory_pool_surface_count (&self->pool) != 0) {
    GST_WARNING_OBJECT (self, "Surfaces leaked: %d",
        gst_va_memory_pool_surface_count (&self->pool));
  }

  G_OBJECT_CLASS (gst_va_allocator_parent_class)->dispose (object);
}

static void
_va_free (GstAllocator * allocator, GstMemory * mem)
{
  GstVaAllocator *self = GST_VA_ALLOCATOR (allocator);
  GstVaMemory *va_mem = (GstVaMemory *) mem;

  if (va_mem->mapped_data) {
    g_warning (G_STRLOC ":%s: Freeing memory %p still mapped", G_STRFUNC,
        va_mem);
    _va_unmap (va_mem);
  }

  if (va_mem->surface != VA_INVALID_ID && mem->parent == NULL) {
    GST_LOG_OBJECT (self, "Destroying surface %#x", va_mem->surface);
    _destroy_surfaces (self->display, &va_mem->surface, 1);
  }

  g_mutex_clear (&va_mem->lock);

  g_slice_free (GstVaMemory, va_mem);
}

static void
gst_va_allocator_class_init (GstVaAllocatorClass * klass)
{
  GstAllocatorClass *allocator_class = GST_ALLOCATOR_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gst_va_allocator_dispose;
  object_class->finalize = gst_va_allocator_finalize;
  allocator_class->free = _va_free;
}

static inline void
_clean_mem (GstVaMemory * mem)
{
  memset (&mem->image, 0, sizeof (mem->image));
  mem->image.image_id = VA_INVALID_ID;
  mem->image.buf = VA_INVALID_ID;

  mem->is_derived = TRUE;
  mem->is_dirty = FALSE;
  mem->prev_mapflags = 0;
  mem->mapped_data = NULL;
}

static void
_reset_mem (GstVaMemory * mem, GstAllocator * allocator, gsize size)
{
  _clean_mem (mem);
  g_atomic_int_set (&mem->map_count, 0);
  g_mutex_init (&mem->lock);

  gst_memory_init (GST_MEMORY_CAST (mem), 0, allocator, NULL, size,
      0 /* align */ , 0 /* offset */ , size);
}

static inline gboolean
_ensure_image (GstVaDisplay * display, VASurfaceID surface,
    GstVideoInfo * info, VAImage * image, gboolean derived)
{
  gboolean ret = TRUE;

  if (image->image_id != VA_INVALID_ID)
    return TRUE;

  if (!_sync_surface (display, surface))
    return FALSE;

  if (derived) {
    ret = _get_derive_image (display, surface, image);
  } else {
    ret = _create_image (display, GST_VIDEO_INFO_FORMAT (info),
        GST_VIDEO_INFO_WIDTH (info), GST_VIDEO_INFO_HEIGHT (info), image);
  }

  return ret;
}

static inline void
_update_info (GstVideoInfo * info, const VAImage * image)
{
  guint i;

  for (i = 0; i < image->num_planes; i++) {
    GST_VIDEO_INFO_PLANE_OFFSET (info, i) = image->offsets[i];
    GST_VIDEO_INFO_PLANE_STRIDE (info, i) = image->pitches[i];
  }

  GST_VIDEO_INFO_SIZE (info) = image->data_size;
}

static inline gboolean
_update_image_info (GstVaAllocator * va_allocator)
{
  VASurfaceID surface;
  VAImage image = {.image_id = VA_INVALID_ID, };

  /* Create a test surface first */
  if (!_create_surfaces (va_allocator->display, va_allocator->rt_format,
          va_allocator->fourcc, GST_VIDEO_INFO_WIDTH (&va_allocator->info),
          GST_VIDEO_INFO_HEIGHT (&va_allocator->info), va_allocator->usage_hint,
          NULL, &surface, 1)) {
    GST_ERROR_OBJECT (va_allocator, "Failed to create a test surface");
    return FALSE;
  }

  GST_DEBUG_OBJECT (va_allocator, "Created surface %#x [%dx%d]", surface,
      GST_VIDEO_INFO_WIDTH (&va_allocator->info),
      GST_VIDEO_INFO_HEIGHT (&va_allocator->info));

  /* Try derived first, but different formats can never derive */
  if (va_allocator->surface_format == va_allocator->img_format) {
    if (_get_derive_image (va_allocator->display, surface, &image)) {
      va_allocator->use_derived = TRUE;
      va_allocator->derived_info = va_allocator->info;
      _update_info (&va_allocator->derived_info, &image);
      _destroy_image (va_allocator->display, image.image_id);
    }
    image.image_id = VA_INVALID_ID;     /* reset it */
  }

  /* Then we try to create a image. */
  if (!_create_image (va_allocator->display, va_allocator->img_format,
          GST_VIDEO_INFO_WIDTH (&va_allocator->info),
          GST_VIDEO_INFO_HEIGHT (&va_allocator->info), &image)) {
    _destroy_surfaces (va_allocator->display, &surface, 1);
    return FALSE;
  }

  _update_info (&va_allocator->info, &image);
  _destroy_image (va_allocator->display, image.image_id);
  _destroy_surfaces (va_allocator->display, &surface, 1);

  return TRUE;
}

static gpointer
_va_map_unlocked (GstVaMemory * mem, GstMapFlags flags)
{
  GstAllocator *allocator = GST_MEMORY_CAST (mem)->allocator;
  GstVideoInfo *info;
  GstVaAllocator *va_allocator;
  GstVaDisplay *display;
  gboolean use_derived;

  g_return_val_if_fail (mem->surface != VA_INVALID_ID, NULL);
  g_return_val_if_fail (GST_IS_VA_ALLOCATOR (allocator), NULL);

  if (g_atomic_int_get (&mem->map_count) > 0) {
    if (!(mem->prev_mapflags & flags) || !mem->mapped_data)
      return NULL;
    else
      goto success;
  }

  va_allocator = GST_VA_ALLOCATOR (allocator);
  display = va_allocator->display;

  if (flags & GST_MAP_WRITE) {
    mem->is_dirty = TRUE;
  } else {                      /* GST_MAP_READ only */
    mem->is_dirty = FALSE;
  }

  if (flags & GST_MAP_VA) {
    mem->mapped_data = &mem->surface;
    goto success;
  }

  switch (gst_va_display_get_implementation (display)) {
    case GST_VA_IMPLEMENTATION_INTEL_IHD:
      /* On Gen7+ Intel graphics the memory is mappable but not
       * cached, so normal memcpy() access is very slow to read, but
       * it's ok for writing. So let's assume that users won't prefer
       * direct-mapped memory if they request read access. */
      use_derived = va_allocator->use_derived && !(flags & GST_MAP_READ);
      break;
    case GST_VA_IMPLEMENTATION_INTEL_I965:
      /* YUV derived images are tiled, so writing them is also
       * problematic */
      use_derived = va_allocator->use_derived && !((flags & GST_MAP_READ)
          || ((flags & GST_MAP_WRITE)
              && GST_VIDEO_INFO_IS_YUV (&va_allocator->derived_info)));
      break;
    case GST_VA_IMPLEMENTATION_MESA_GALLIUM:
      /* Reading RGB derived images, with non-standard resolutions,
       * looks like tiled too. TODO(victor): fill a bug in Mesa. */
      use_derived = va_allocator->use_derived && !((flags & GST_MAP_READ)
          && GST_VIDEO_INFO_IS_RGB (&va_allocator->derived_info));
      break;
    default:
      use_derived = va_allocator->use_derived;
      break;
  }
  if (use_derived)
    info = &va_allocator->derived_info;
  else
    info = &va_allocator->info;

  if (!_ensure_image (display, mem->surface, info, &mem->image, use_derived))
    return NULL;

  mem->is_derived = use_derived;

  if (!mem->is_derived) {
    if (!_get_image (display, mem->surface, &mem->image))
      goto fail;
  }

  if (!_map_buffer (display, mem->image.buf, &mem->mapped_data))
    goto fail;

success:
  {
    mem->prev_mapflags = flags;
    g_atomic_int_add (&mem->map_count, 1);
    return mem->mapped_data;
  }

fail:
  {
    _destroy_image (display, mem->image.image_id);
    _clean_mem (mem);
    return NULL;
  }
}

static gpointer
_va_map (GstVaMemory * mem, gsize maxsize, GstMapFlags flags)
{
  gpointer data;

  g_mutex_lock (&mem->lock);
  data = _va_map_unlocked (mem, flags);
  g_mutex_unlock (&mem->lock);

  return data;
}

static gboolean
_va_unmap_unlocked (GstVaMemory * mem)
{
  GstAllocator *allocator = GST_MEMORY_CAST (mem)->allocator;
  GstVaDisplay *display;
  gboolean ret = TRUE;

  if (!g_atomic_int_dec_and_test (&mem->map_count))
    return TRUE;

  if (mem->prev_mapflags & GST_MAP_VA)
    goto bail;

  display = GST_VA_ALLOCATOR (allocator)->display;

  if (mem->image.image_id != VA_INVALID_ID) {
    if (mem->is_dirty && !mem->is_derived) {
      ret = _put_image (display, mem->surface, &mem->image);
      mem->is_dirty = FALSE;
    }
    /* XXX(victor): if is derived and is dirty, create another surface
     * an replace it in mem */
  }

  ret &= _unmap_buffer (display, mem->image.buf);
  ret &= _destroy_image (display, mem->image.image_id);

bail:
  _clean_mem (mem);

  return ret;
}

static gboolean
_va_unmap (GstVaMemory * mem)
{
  gboolean ret;

  g_mutex_lock (&mem->lock);
  ret = _va_unmap_unlocked (mem);
  g_mutex_unlock (&mem->lock);

  return ret;
}

static GstMemory *
_va_share (GstMemory * mem, gssize offset, gssize size)
{
  GstVaMemory *vamem = (GstVaMemory *) mem;
  GstVaMemory *sub;
  GstMemory *parent;

  GST_DEBUG ("%p: share %" G_GSSIZE_FORMAT ", %" G_GSIZE_FORMAT, mem, offset,
      size);

  /* find real parent */
  if ((parent = vamem->mem.parent) == NULL)
    parent = (GstMemory *) vamem;

  if (size == -1)
    size = mem->maxsize - offset;

  sub = g_slice_new (GstVaMemory);

  /* the shared memory is alwyas readonly */
  gst_memory_init (GST_MEMORY_CAST (sub), GST_MINI_OBJECT_FLAGS (parent) |
      GST_MINI_OBJECT_FLAG_LOCK_READONLY, vamem->mem.allocator, parent,
      vamem->mem.maxsize, vamem->mem.align, vamem->mem.offset + offset, size);

  sub->surface = vamem->surface;
  sub->surface_format = vamem->surface_format;

  _clean_mem (sub);

  g_atomic_int_set (&sub->map_count, 0);
  g_mutex_init (&sub->lock);

  return GST_MEMORY_CAST (sub);
}

/* XXX(victor): deep copy implementation. A further optimization can
 * be done with vaCopy() from libva 2.12 */
static GstMemory *
_va_copy (GstMemory * mem, gssize offset, gssize size)
{
  GstMemory *copy;
  GstMapInfo sinfo, dinfo;
  GstVaAllocator *va_allocator = GST_VA_ALLOCATOR (mem->allocator);

  GST_DEBUG ("%p: copy %" G_GSSIZE_FORMAT ", %" G_GSIZE_FORMAT, mem, offset,
      size);

  {
    GST_VA_MEMORY_POOL_LOCK (&va_allocator->pool);
    copy = gst_va_memory_pool_pop (&va_allocator->pool);
    GST_VA_MEMORY_POOL_UNLOCK (&va_allocator->pool);

    if (!copy) {
      copy = gst_va_allocator_alloc (mem->allocator);
      if (!copy) {
        GST_WARNING ("failed to allocate new memory");
        return NULL;
      }
    } else {
      gst_object_ref (mem->allocator);
    }
  }

  if (!gst_memory_map (mem, &sinfo, GST_MAP_READ))
    return NULL;

  if (size == -1)
    size = sinfo.size > offset ? sinfo.size - offset : 0;

  if (offset == 0 && size == sinfo.size) {
    GstVaMemory *va_mem = (GstVaMemory *) mem;
    GstVaMemory *va_copy = (GstVaMemory *) copy;

    if (!va_mem->is_derived) {
      if (_put_image (va_allocator->display, va_copy->surface, &va_mem->image)) {
        GST_LOG ("shallow copy of %#x to %#x", va_mem->surface,
            va_copy->surface);
        gst_memory_unmap (mem, &sinfo);
        return copy;
      }
    }
  }

  if (!gst_memory_map (copy, &dinfo, GST_MAP_WRITE)) {
    GST_WARNING ("could not write map memory %p", copy);
    gst_allocator_free (mem->allocator, copy);
    gst_memory_unmap (mem, &sinfo);
    return NULL;
  }

  memcpy (dinfo.data, sinfo.data + offset, size);
  gst_memory_unmap (copy, &dinfo);
  gst_memory_unmap (mem, &sinfo);

  return copy;
}

static void
gst_va_allocator_init (GstVaAllocator * self)
{
  GstAllocator *allocator = GST_ALLOCATOR (self);

  allocator->mem_type = GST_ALLOCATOR_VASURFACE;
  allocator->mem_map = (GstMemoryMapFunction) _va_map;
  allocator->mem_unmap = (GstMemoryUnmapFunction) _va_unmap;
  allocator->mem_share = _va_share;
  allocator->mem_copy = _va_copy;

  gst_va_memory_pool_init (&self->pool);

  GST_OBJECT_FLAG_SET (self, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

static gboolean
gst_va_memory_release (GstMiniObject * mini_object)
{
  GstMemory *mem = GST_MEMORY_CAST (mini_object);
  GstVaAllocator *self = GST_VA_ALLOCATOR (mem->allocator);

  GST_LOG ("releasing %p: surface %#x", mem, gst_va_memory_get_surface (mem));

  gst_va_memory_pool_push (&self->pool, mem);

  /* Keep last in case we are holding on the last allocator ref */
  gst_object_unref (mem->allocator);

  /* don't call mini_object's free */
  return FALSE;
}

GstMemory *
gst_va_allocator_alloc (GstAllocator * allocator)
{
  GstVaAllocator *self;
  GstVaMemory *mem;
  VASurfaceID surface;

  g_return_val_if_fail (GST_IS_VA_ALLOCATOR (allocator), NULL);

  self = GST_VA_ALLOCATOR (allocator);

  if (self->rt_format == 0) {
    GST_ERROR_OBJECT (self, "Unknown fourcc or chroma format");
    return NULL;
  }

  if (!_create_surfaces (self->display, self->rt_format, self->fourcc,
          GST_VIDEO_INFO_WIDTH (&self->info),
          GST_VIDEO_INFO_HEIGHT (&self->info), self->usage_hint, NULL,
          &surface, 1))
    return NULL;

  mem = g_slice_new (GstVaMemory);

  mem->surface = surface;
  mem->surface_format = self->surface_format;

  _reset_mem (mem, allocator, GST_VIDEO_INFO_SIZE (&self->info));

  GST_MINI_OBJECT (mem)->dispose = gst_va_memory_release;
  gst_va_memory_pool_surface_inc (&self->pool);

  GST_LOG_OBJECT (self, "Created surface %#x [%dx%d]", mem->surface,
      GST_VIDEO_INFO_WIDTH (&self->info), GST_VIDEO_INFO_HEIGHT (&self->info));

  return GST_MEMORY_CAST (mem);
}

GstAllocator *
gst_va_allocator_new (GstVaDisplay * display, GArray * surface_formats)
{
  GstVaAllocator *self;

  g_return_val_if_fail (GST_IS_VA_DISPLAY (display), NULL);

  self = g_object_new (GST_TYPE_VA_ALLOCATOR, NULL);
  self->display = gst_object_ref (display);
  self->surface_formats = surface_formats;
  gst_object_ref_sink (self);

  return GST_ALLOCATOR (self);
}

gboolean
gst_va_allocator_setup_buffer (GstAllocator * allocator, GstBuffer * buffer)
{
  GstMemory *mem = gst_va_allocator_alloc (allocator);
  if (!mem)
    return FALSE;

  gst_buffer_append_memory (buffer, mem);
  return TRUE;
}

static VASurfaceID
gst_va_allocator_prepare_buffer_unlocked (GstVaAllocator * self,
    GstBuffer * buffer)
{
  GstMemory *mem;
  VASurfaceID surface;

  mem = gst_va_memory_pool_pop (&self->pool);
  if (!mem)
    return VA_INVALID_ID;

  gst_object_ref (mem->allocator);
  surface = gst_va_memory_get_surface (mem);
  gst_buffer_append_memory (buffer, mem);

  GST_LOG ("buffer %p: memory %p - surface %#x", buffer, mem, surface);

  return surface;
}

gboolean
gst_va_allocator_prepare_buffer (GstAllocator * allocator, GstBuffer * buffer)
{
  GstVaAllocator *self = GST_VA_ALLOCATOR (allocator);
  VASurfaceID surface;

  GST_VA_MEMORY_POOL_LOCK (&self->pool);
  surface = gst_va_allocator_prepare_buffer_unlocked (self, buffer);
  GST_VA_MEMORY_POOL_UNLOCK (&self->pool);

  return (surface != VA_INVALID_ID);
}

void
gst_va_allocator_flush (GstAllocator * allocator)
{
  GstVaAllocator *self = GST_VA_ALLOCATOR (allocator);

  gst_va_memory_pool_flush (&self->pool, self->display);
}

static gboolean
gst_va_allocator_try (GstAllocator * allocator)
{
  GstVaAllocator *self = GST_VA_ALLOCATOR (allocator);

  self->fourcc = 0;
  self->rt_format = 0;
  self->use_derived = FALSE;
  self->img_format = GST_VIDEO_INFO_FORMAT (&self->info);

  self->surface_format =
      gst_va_video_surface_format_from_image_format (self->img_format,
      self->surface_formats);
  if (self->surface_format == GST_VIDEO_FORMAT_UNKNOWN) {
    /* try a surface without fourcc but rt_format only */
    self->fourcc = 0;
    self->rt_format = gst_va_chroma_from_video_format (self->img_format);
  } else {
    self->fourcc = gst_va_fourcc_from_video_format (self->surface_format);
    self->rt_format = gst_va_chroma_from_video_format (self->surface_format);
  }

  if (self->rt_format == 0) {
    GST_ERROR_OBJECT (allocator, "Unsupported image format: %s",
        gst_video_format_to_string (self->img_format));
    return FALSE;
  }

  if (!_update_image_info (self)) {
    GST_ERROR_OBJECT (allocator, "Failed to update allocator info");
    return FALSE;
  }

  GST_INFO_OBJECT (self,
      "va allocator info, surface format: %s, image format: %s, "
      "use derived: %s, rt format: 0x%x, fourcc: %" GST_FOURCC_FORMAT,
      (self->surface_format == GST_VIDEO_FORMAT_UNKNOWN) ? "unknown"
      : gst_video_format_to_string (self->surface_format),
      gst_video_format_to_string (self->img_format),
      self->use_derived ? "true" : "false", self->rt_format,
      GST_FOURCC_ARGS (self->fourcc));
  return TRUE;
}

gboolean
gst_va_allocator_set_format (GstAllocator * allocator, GstVideoInfo * info,
    guint usage_hint)
{
  GstVaAllocator *self;
  gboolean ret;

  g_return_val_if_fail (GST_IS_VA_ALLOCATOR (allocator), FALSE);
  g_return_val_if_fail (info, FALSE);

  self = GST_VA_ALLOCATOR (allocator);

  if (gst_va_memory_pool_surface_count (&self->pool) != 0) {
    if (GST_VIDEO_INFO_FORMAT (info) == GST_VIDEO_INFO_FORMAT (&self->info)
        && GST_VIDEO_INFO_WIDTH (info) == GST_VIDEO_INFO_WIDTH (&self->info)
        && GST_VIDEO_INFO_HEIGHT (info) == GST_VIDEO_INFO_HEIGHT (&self->info)
        && usage_hint == self->usage_hint) {
      *info = self->info;       /* update callee info (offset & stride) */
      return TRUE;
    }
    return FALSE;
  }

  self->usage_hint = usage_hint;
  self->info = *info;

  ret = gst_va_allocator_try (allocator);
  if (ret)
    *info = self->info;

  return ret;
}

gboolean
gst_va_allocator_get_format (GstAllocator * allocator, GstVideoInfo * info,
    guint * usage_hint)
{
  GstVaAllocator *self = GST_VA_ALLOCATOR (allocator);

  if (GST_VIDEO_INFO_FORMAT (&self->info) == GST_VIDEO_FORMAT_UNKNOWN)
    return FALSE;

  if (info)
    *info = self->info;
  if (usage_hint)
    *usage_hint = self->usage_hint;

  return TRUE;
}

/*============ Utilities =====================================================*/

VASurfaceID
gst_va_memory_get_surface (GstMemory * mem)
{
  VASurfaceID surface = VA_INVALID_ID;

  if (!mem->allocator)
    return VA_INVALID_ID;

  if (GST_IS_DMABUF_ALLOCATOR (mem->allocator)) {
    GstVaBufferSurface *buf;

    buf = gst_mini_object_get_qdata (GST_MINI_OBJECT (mem),
        gst_va_buffer_surface_quark ());
    if (buf)
      surface = buf->surface;
  } else if (GST_IS_VA_ALLOCATOR (mem->allocator)) {
    GstVaMemory *va_mem = (GstVaMemory *) mem;
    surface = va_mem->surface;
  }

  return surface;
}

VASurfaceID
gst_va_buffer_get_surface (GstBuffer * buffer)
{
  GstMemory *mem;

  mem = gst_buffer_peek_memory (buffer, 0);
  if (!mem)
    return VA_INVALID_ID;

  return gst_va_memory_get_surface (mem);
}

gboolean
gst_va_buffer_create_aux_surface (GstBuffer * buffer)
{
  GstMemory *mem;
  VASurfaceID surface = VA_INVALID_ID;
  GstVaDisplay *display = NULL;
  GstVideoFormat format;
  gint width, height;
  GstVaBufferSurface *surface_buffer;

  mem = gst_buffer_peek_memory (buffer, 0);
  if (!mem)
    return FALSE;

  /* Already created it. */
  surface_buffer = gst_mini_object_get_qdata (GST_MINI_OBJECT (mem),
      gst_va_buffer_aux_surface_quark ());
  if (surface_buffer)
    return TRUE;

  if (!mem->allocator)
    return FALSE;

  if (GST_IS_VA_DMABUF_ALLOCATOR (mem->allocator)) {
    GstVaDmabufAllocator *self = GST_VA_DMABUF_ALLOCATOR (mem->allocator);
    guint32 fourcc, rt_format;

    format = GST_VIDEO_INFO_FORMAT (&self->info);
    fourcc = gst_va_fourcc_from_video_format (format);
    rt_format = gst_va_chroma_from_video_format (format);
    if (fourcc == 0 || rt_format == 0) {
      GST_ERROR_OBJECT (self, "Unsupported format: %s",
          gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (&self->info)));
      return FALSE;
    }

    display = self->display;
    width = GST_VIDEO_INFO_WIDTH (&self->info);
    height = GST_VIDEO_INFO_HEIGHT (&self->info);
    if (!_create_surfaces (self->display, rt_format, fourcc,
            GST_VIDEO_INFO_WIDTH (&self->info),
            GST_VIDEO_INFO_HEIGHT (&self->info), self->usage_hint, NULL,
            &surface, 1))
      return FALSE;
  } else if (GST_IS_VA_ALLOCATOR (mem->allocator)) {
    GstVaAllocator *self = GST_VA_ALLOCATOR (mem->allocator);

    if (self->rt_format == 0) {
      GST_ERROR_OBJECT (self, "Unknown fourcc or chroma format");
      return FALSE;
    }

    display = self->display;
    width = GST_VIDEO_INFO_WIDTH (&self->info);
    height = GST_VIDEO_INFO_HEIGHT (&self->info);
    format = GST_VIDEO_INFO_FORMAT (&self->info);
    if (!_create_surfaces (self->display, self->rt_format, self->fourcc,
            GST_VIDEO_INFO_WIDTH (&self->info),
            GST_VIDEO_INFO_HEIGHT (&self->info), self->usage_hint, NULL,
            &surface, 1))
      return FALSE;
  } else {
    g_assert_not_reached ();
  }

  if (!display || surface == VA_INVALID_ID)
    return FALSE;

  surface_buffer = gst_va_buffer_surface_new (surface, format, width, height);
  surface_buffer->display = gst_object_ref (display);
  g_atomic_int_add (&surface_buffer->ref_count, 1);

  gst_mini_object_set_qdata (GST_MINI_OBJECT (mem),
      gst_va_buffer_aux_surface_quark (), surface_buffer,
      gst_va_buffer_surface_unref);

  return TRUE;
}

VASurfaceID
gst_va_buffer_get_aux_surface (GstBuffer * buffer)
{
  GstVaBufferSurface *surface_buffer;
  GstMemory *mem;

  mem = gst_buffer_peek_memory (buffer, 0);
  if (!mem)
    return VA_INVALID_ID;

  surface_buffer = gst_mini_object_get_qdata (GST_MINI_OBJECT (mem),
      gst_va_buffer_aux_surface_quark ());
  if (!surface_buffer)
    return VA_INVALID_ID;

  /* No one increments it, and its lifetime is the same with the
     gstmemory itself */
  g_assert (g_atomic_int_get (&surface_buffer->ref_count) == 1);

  return surface_buffer->surface;
}
