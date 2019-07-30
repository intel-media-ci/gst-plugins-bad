/* GStreamer Intel MSDK plugin
 * Copyright (c) 2016, Oblong Industries, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "msdk.h"
#include "gstmsdkvideomemory.h"
#include "gstmsdksystemmemory.h"

GST_DEBUG_CATEGORY_EXTERN (gst_msdk_debug);
#define GST_CAT_DEFAULT gst_msdk_debug

#define INVALID_INDEX         ((guint) -1)
#define GST_MSDK_ALIGNMENT_PADDING(num,padding) ((padding) - ((num) & ((padding) - 1)))

struct map
{
  GstVideoFormat format;
  mfxU16 mfx_chroma_format;
  mfxU32 mfx_fourcc;
};

#define GST_VIDEO_INFO_TO_MFX_MAP(FORMAT, CHROMA, FOURCC) \
    { GST_VIDEO_FORMAT_##FORMAT, MFX_CHROMAFORMAT_##CHROMA, MFX_FOURCC_##FOURCC }

static const struct map gst_msdk_video_format_to_mfx_map[] = {
  GST_VIDEO_INFO_TO_MFX_MAP (NV12, YUV420, NV12),
  GST_VIDEO_INFO_TO_MFX_MAP (YV12, YUV420, YV12),
  GST_VIDEO_INFO_TO_MFX_MAP (I420, YUV420, YV12),
  GST_VIDEO_INFO_TO_MFX_MAP (P010_10LE, YUV420, P010),
  GST_VIDEO_INFO_TO_MFX_MAP (YUY2, YUV422, YUY2),
  GST_VIDEO_INFO_TO_MFX_MAP (UYVY, YUV422, UYVY),
  GST_VIDEO_INFO_TO_MFX_MAP (BGRA, YUV444, RGB4),
  GST_VIDEO_INFO_TO_MFX_MAP (BGRx, YUV444, RGB4),
#if (MFX_VERSION >= 1028)
  GST_VIDEO_INFO_TO_MFX_MAP (RGB16, YUV444, RGB565),
#endif
  GST_VIDEO_INFO_TO_MFX_MAP (VUYA, YUV444, AYUV),
  GST_VIDEO_INFO_TO_MFX_MAP (BGR10A2_LE, YUV444, A2RGB10),
#if (MFX_VERSION >= 1027)
  GST_VIDEO_INFO_TO_MFX_MAP (Y210, YUV422, Y210),
  GST_VIDEO_INFO_TO_MFX_MAP (Y410, YUV444, Y410),
#endif
  {0, 0, 0}
};

const gchar *
msdk_status_to_string (mfxStatus status)
{
  switch (status) {
      /* no error */
    case MFX_ERR_NONE:
      return "no error";
      /* reserved for unexpected errors */
    case MFX_ERR_UNKNOWN:
      return "unknown error";
      /* error codes <0 */
    case MFX_ERR_NULL_PTR:
      return "null pointer";
    case MFX_ERR_UNSUPPORTED:
      return "undeveloped feature";
    case MFX_ERR_MEMORY_ALLOC:
      return "failed to allocate memory";
    case MFX_ERR_NOT_ENOUGH_BUFFER:
      return "insufficient buffer at input/output";
    case MFX_ERR_INVALID_HANDLE:
      return "invalid handle";
    case MFX_ERR_LOCK_MEMORY:
      return "failed to lock the memory block";
    case MFX_ERR_NOT_INITIALIZED:
      return "member function called before initialization";
    case MFX_ERR_NOT_FOUND:
      return "the specified object is not found";
    case MFX_ERR_MORE_DATA:
      return "expect more data at input";
    case MFX_ERR_MORE_SURFACE:
      return "expect more surface at output";
    case MFX_ERR_ABORTED:
      return "operation aborted";
    case MFX_ERR_DEVICE_LOST:
      return "lose the HW acceleration device";
    case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM:
      return "incompatible video parameters";
    case MFX_ERR_INVALID_VIDEO_PARAM:
      return "invalid video parameters";
    case MFX_ERR_UNDEFINED_BEHAVIOR:
      return "undefined behavior";
    case MFX_ERR_DEVICE_FAILED:
      return "device operation failure";
    case MFX_ERR_MORE_BITSTREAM:
      return "expect more bitstream buffers at output";
    case MFX_ERR_INCOMPATIBLE_AUDIO_PARAM:
      return "incompatible audio parameters";
    case MFX_ERR_INVALID_AUDIO_PARAM:
      return "invalid audio parameters";
      /* warnings >0 */
    case MFX_WRN_IN_EXECUTION:
      return "the previous asynchronous operation is in execution";
    case MFX_WRN_DEVICE_BUSY:
      return "the HW acceleration device is busy";
    case MFX_WRN_VIDEO_PARAM_CHANGED:
      return "the video parameters are changed during decoding";
    case MFX_WRN_PARTIAL_ACCELERATION:
      return "SW is used";
    case MFX_WRN_INCOMPATIBLE_VIDEO_PARAM:
      return "incompatible video parameters";
    case MFX_WRN_VALUE_NOT_CHANGED:
      return "the value is saturated based on its valid range";
    case MFX_WRN_OUT_OF_RANGE:
      return "the value is out of valid range";
    case MFX_WRN_FILTER_SKIPPED:
      return "one of requested filters has been skipped";
    case MFX_WRN_INCOMPATIBLE_AUDIO_PARAM:
      return "incompatible audio parameters";
    default:
      break;
  }
  return "undefiend error";
}

void
msdk_close_session (mfxSession session)
{
  mfxStatus status;

  if (!session)
    return;

  status = MFXClose (session);
  if (status != MFX_ERR_NONE)
    GST_ERROR ("Close failed (%s)", msdk_status_to_string (status));
}

mfxSession
msdk_open_session (mfxIMPL impl)
{
  mfxSession session = NULL;
  mfxVersion version = { {1, 1}
  };
  mfxIMPL implementation;
  mfxStatus status;

  static const gchar *implementation_names[] = {
    "AUTO", "SOFTWARE", "HARDWARE", "AUTO_ANY", "HARDWARE_ANY", "HARDWARE2",
    "HARDWARE3", "HARDWARE4", "RUNTIME"
  };

  status = MFXInit (impl, &version, &session);
  if (status != MFX_ERR_NONE) {
    GST_ERROR ("Intel Media SDK not available (%s)",
        msdk_status_to_string (status));
    goto failed;
  }

  status = MFXQueryIMPL (session, &implementation);
  if (status != MFX_ERR_NONE) {
    GST_ERROR ("Query implementation failed (%s)",
        msdk_status_to_string (status));
    goto failed;
  }

  status = MFXQueryVersion (session, &version);
  if (status != MFX_ERR_NONE) {
    GST_ERROR ("Query version failed (%s)", msdk_status_to_string (status));
    goto failed;
  }

  GST_INFO ("MSDK implementation: 0x%04x (%s)", implementation,
      implementation_names[MFX_IMPL_BASETYPE (implementation)]);
  GST_INFO ("MSDK version: %d.%d", version.Major, version.Minor);

  return session;

failed:
  msdk_close_session (session);
  return NULL;
}

gboolean
msdk_is_available (void)
{
  mfxSession session = msdk_open_session (MFX_IMPL_AUTO_ANY);
  if (!session) {
    return FALSE;
  }

  msdk_close_session (session);
  return TRUE;
}

void
gst_msdk_set_video_alignment (GstVideoInfo * info, guint alloc_w, guint alloc_h,
    GstVideoAlignment * alignment)
{
  guint i, width, height;

  width = GST_VIDEO_INFO_WIDTH (info);
  height = GST_VIDEO_INFO_HEIGHT (info);

  g_assert (alloc_w == 0 || alloc_w >= width);
  g_assert (alloc_h == 0 || alloc_h >= height);

  if (alloc_w == 0)
    alloc_w = width;

  if (alloc_h == 0)
    alloc_h = height;

  gst_video_alignment_reset (alignment);
  for (i = 0; i < GST_VIDEO_INFO_N_PLANES (info); i++)
    alignment->stride_align[i] = 15;    /* 16-byte alignment */

  alignment->padding_right = GST_ROUND_UP_16 (alloc_w) - width;
  alignment->padding_bottom = GST_ROUND_UP_32 (alloc_h) - height;
}

static const struct map *
_map_lookup_format (GstVideoFormat format)
{
  const struct map *m = gst_msdk_video_format_to_mfx_map;

  for (; m->format != 0; m++) {
    if (m->format == format)
      return m;
  }
  return NULL;
}

gint
gst_msdk_get_mfx_chroma_from_format (GstVideoFormat format)
{
  const struct map *const m = _map_lookup_format (format);

  return m ? m->mfx_chroma_format : -1;
}

gint
gst_msdk_get_mfx_fourcc_from_format (GstVideoFormat format)
{
  const struct map *const m = _map_lookup_format (format);

  return m ? m->mfx_fourcc : -1;
}

void
gst_msdk_set_mfx_frame_info_from_video_info (mfxFrameInfo * mfx_info,
    GstVideoInfo * info)
{
  g_return_if_fail (info && mfx_info);

  /* Use the first component in info to calculate mfx width / height */
  mfx_info->Width =
      GST_ROUND_UP_16 (GST_VIDEO_INFO_COMP_STRIDE (info,
          0) / GST_VIDEO_INFO_COMP_PSTRIDE (info, 0));

  if (GST_VIDEO_INFO_N_PLANES (info) > 1)
    mfx_info->Height =
        GST_ROUND_UP_32 (GST_VIDEO_INFO_COMP_OFFSET (info,
            1) / GST_VIDEO_INFO_COMP_STRIDE (info, 0));
  else
    mfx_info->Height =
        GST_ROUND_UP_32 (GST_VIDEO_INFO_SIZE (info) /
        GST_VIDEO_INFO_COMP_STRIDE (info, 0));

  mfx_info->CropW = GST_VIDEO_INFO_WIDTH (info);
  mfx_info->CropH = GST_VIDEO_INFO_HEIGHT (info);
  mfx_info->FrameRateExtN = GST_VIDEO_INFO_FPS_N (info);
  mfx_info->FrameRateExtD = GST_VIDEO_INFO_FPS_D (info);
  mfx_info->AspectRatioW = GST_VIDEO_INFO_PAR_N (info);
  mfx_info->AspectRatioH = GST_VIDEO_INFO_PAR_D (info);
  mfx_info->PicStruct =
      !GST_VIDEO_INFO_IS_INTERLACED (info) ? MFX_PICSTRUCT_PROGRESSIVE :
      MFX_PICSTRUCT_UNKNOWN;
  mfx_info->FourCC =
      gst_msdk_get_mfx_fourcc_from_format (GST_VIDEO_INFO_FORMAT (info));
  mfx_info->ChromaFormat =
      gst_msdk_get_mfx_chroma_from_format (GST_VIDEO_INFO_FORMAT (info));

  switch (mfx_info->FourCC) {
    case MFX_FOURCC_P010:
#if (MFX_VERSION >= 1027)
    case MFX_FOURCC_Y210:
#endif
      mfx_info->BitDepthLuma = 10;
      mfx_info->BitDepthChroma = 10;
      mfx_info->Shift = 1;

      break;

#if (MFX_VERSION >= 1027)
    case MFX_FOURCC_Y410:
      mfx_info->BitDepthLuma = 10;
      mfx_info->BitDepthChroma = 10;
      mfx_info->Shift = 0;

      break;
#endif

    default:
      break;
  }

  return;
}

gboolean
gst_msdk_is_msdk_buffer (GstBuffer * buf)
{
  GstAllocator *allocator;
  GstMemory *mem = gst_buffer_peek_memory (buf, 0);

  allocator = GST_MEMORY_CAST (mem)->allocator;

  if (allocator && (GST_IS_MSDK_VIDEO_ALLOCATOR (allocator) ||
          GST_IS_MSDK_SYSTEM_ALLOCATOR (allocator) ||
          GST_IS_MSDK_DMABUF_ALLOCATOR (allocator)))
    return TRUE;
  else
    return FALSE;
}

mfxFrameSurface1 *
gst_msdk_get_surface_from_buffer (GstBuffer * buf)
{
  GstAllocator *allocator;
  GstMemory *mem = gst_buffer_peek_memory (buf, 0);

  allocator = GST_MEMORY_CAST (mem)->allocator;

  if (GST_IS_MSDK_VIDEO_ALLOCATOR (allocator))
    return GST_MSDK_VIDEO_MEMORY_CAST (mem)->surface;
  else if (GST_IS_MSDK_SYSTEM_ALLOCATOR (allocator))
    return GST_MSDK_SYSTEM_MEMORY_CAST (mem)->surface;
  else if (GST_IS_MSDK_DMABUF_ALLOCATOR (allocator)) {
    return gst_mini_object_get_qdata (GST_MINI_OBJECT (mem),
        g_quark_from_static_string ("GstMsdkBufferSurface"));
  }

  return NULL;
}

GstVideoFormat
gst_msdk_get_video_format_from_mfx_fourcc (mfxU32 fourcc)
{
  const struct map *m = gst_msdk_video_format_to_mfx_map;

  for (; m->mfx_fourcc != 0; m++) {
    if (m->mfx_fourcc == fourcc)
      return m->format;
  }

  return GST_VIDEO_FORMAT_UNKNOWN;
}

void
gst_msdk_update_mfx_frame_info_from_mfx_video_param (mfxFrameInfo * mfx_info,
    mfxVideoParam * param)
{
  mfx_info->BitDepthLuma = param->mfx.FrameInfo.BitDepthLuma;
  mfx_info->BitDepthChroma = param->mfx.FrameInfo.BitDepthChroma;
  mfx_info->Shift = param->mfx.FrameInfo.Shift;
}
