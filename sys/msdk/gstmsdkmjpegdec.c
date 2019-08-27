/* GStreamer Intel MSDK plugin
 * Copyright (c) 2016, Intel Corporation
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
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGDECE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <mfxstructures.h>
#include <mfxjpeg.h>

#include "gstmsdkmjpegdec.h"
#include "gstmsdkvideomemory.h"

#include <gst/pbutils/pbutils.h>

GST_DEBUG_CATEGORY_EXTERN (gst_msdkmjpegdec_debug);
#define GST_CAT_DEFAULT gst_msdkmjpegdec_debug

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("image/jpeg, "
        "width = (int) [ 1, MAX ], height = (int) [ 1, MAX ], parsed = true ")
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_MSDK_CAPS_STR ("{ NV12, YUY2 }", "{ NV12, YUY2 }"))
    );

#define gst_msdkmjpegdec_parent_class parent_class
G_DEFINE_TYPE (GstMsdkMJPEGDec, gst_msdkmjpegdec, GST_TYPE_MSDKDEC);

static gboolean
gst_msdkmjpegdec_configure (GstMsdkDec * decoder)
{
  decoder->param.mfx.CodecId = MFX_CODEC_JPEG;

  /* HACK to make sure MSDK won't crash while handling non-interleaved samples */
  /* setting MFX_SCANTYPE_UNKNOWN (== 0) causing issues for
     non-interleaved samples. Usage of MFXVideoDECODE_DecodeHeader
     also doesn't seems to fix the issue. But even if we hardcode
     the InterleaveDec to MFX_SCANTYPE_NONINTERLEAVED, msdk seems to be taking care
     of Interleaved samples, so let's hardcode it for now */
  decoder->param.mfx.InterleavedDec = MFX_SCANTYPE_NONINTERLEAVED;

  return TRUE;
}

static gboolean
gst_msdkmjpegdec_post_configure (GstMsdkDec * decoder)
{
  /* Set the output color format based on the input color format */
  switch (decoder->param.mfx.JPEGChromaFormat) {
    case MFX_CHROMAFORMAT_YUV422:
      decoder->param.mfx.FrameInfo.FourCC = MFX_FOURCC_YUY2;
      decoder->param.mfx.FrameInfo.ChromaFormat =
          decoder->param.mfx.JPEGChromaFormat;
      break;
    default:
      break;
  }

  return TRUE;
}

static void
gst_msdkmjpegdec_class_init (GstMsdkMJPEGDecClass * klass)
{
  GstElementClass *element_class;
  GstMsdkDecClass *decoder_class;

  element_class = GST_ELEMENT_CLASS (klass);
  decoder_class = GST_MSDKDEC_CLASS (klass);

  decoder_class->configure = GST_DEBUG_FUNCPTR (gst_msdkmjpegdec_configure);
  decoder_class->post_configure =
      GST_DEBUG_FUNCPTR (gst_msdkmjpegdec_post_configure);

  gst_element_class_set_static_metadata (element_class,
      "Intel MSDK MJPEG decoder",
      "Codec/Decoder/Video/Hardware",
      "MJPEG video decoder based on Intel Media SDK",
      "Scott D Phillips <scott.d.phillips@intel.com>");

  gst_element_class_add_static_pad_template (element_class, &sink_factory);
  gst_element_class_add_static_pad_template (element_class, &src_factory);
}

static void
gst_msdkmjpegdec_init (GstMsdkMJPEGDec * thiz)
{
}
