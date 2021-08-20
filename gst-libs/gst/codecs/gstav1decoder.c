/* GStreamer
 * Copyright (C) 2020 He Junyan <junyan.he@intel.com>
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

/**
 * SECTION:gstav1decoder
 * @title: Gstav1Decoder
 * @short_description: Base class to implement stateless AV1 decoders
 * @sources:
 * - gstav1picture.h
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstav1decoder.h"

GST_DEBUG_CATEGORY (gst_av1_decoder_debug);
#define GST_CAT_DEFAULT gst_av1_decoder_debug

/* properties */
#define DEFAULT_OPPOINT 0

enum
{
  PROP_0,
  PROP_OPPOINT
};

struct _GstAV1DecoderPrivate
{
  gint max_width;
  gint max_height;
  guint32 operating_point;
  GstAV1Profile profile;
  GstAV1Parser *parser;
  GstAV1Dpb *dpb;
  GstAV1Picture *current_picture;
  GstVideoCodecFrame *current_frame;
};

#define parent_class gst_av1_decoder_parent_class
G_DEFINE_ABSTRACT_TYPE_WITH_CODE (GstAV1Decoder, gst_av1_decoder,
    GST_TYPE_VIDEO_DECODER,
    G_ADD_PRIVATE (GstAV1Decoder);
    GST_DEBUG_CATEGORY_INIT (gst_av1_decoder_debug, "av1decoder", 0,
        "AV1 Video Decoder"));

static gint
_floor_log2 (guint32 x)
{
  gint s = 0;

  while (x != 0) {
    x = x >> 1;
    s++;
  }
  return s - 1;
}

static gboolean gst_av1_decoder_start (GstVideoDecoder * decoder);
static gboolean gst_av1_decoder_stop (GstVideoDecoder * decoder);
static gboolean gst_av1_decoder_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state);
static GstFlowReturn gst_av1_decoder_finish (GstVideoDecoder * decoder);
static gboolean gst_av1_decoder_flush (GstVideoDecoder * decoder);
static GstFlowReturn gst_av1_decoder_drain (GstVideoDecoder * decoder);
static GstFlowReturn gst_av1_decoder_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame);

static GstAV1Picture *gst_av1_decoder_duplicate_picture_default (GstAV1Decoder *
    decoder, GstAV1Picture * picture);

static void gst_av1_decoder_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec);
static void gst_av1_decoder_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec);

static void
gst_av1_decoder_class_init (GstAV1DecoderClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  GstVideoDecoderClass *decoder_class = GST_VIDEO_DECODER_CLASS (klass);

  gobject_class->get_property = gst_av1_decoder_get_property;
  gobject_class->set_property = gst_av1_decoder_set_property;

  decoder_class->start = GST_DEBUG_FUNCPTR (gst_av1_decoder_start);
  decoder_class->stop = GST_DEBUG_FUNCPTR (gst_av1_decoder_stop);
  decoder_class->set_format = GST_DEBUG_FUNCPTR (gst_av1_decoder_set_format);
  decoder_class->finish = GST_DEBUG_FUNCPTR (gst_av1_decoder_finish);
  decoder_class->flush = GST_DEBUG_FUNCPTR (gst_av1_decoder_flush);
  decoder_class->drain = GST_DEBUG_FUNCPTR (gst_av1_decoder_drain);
  decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_av1_decoder_handle_frame);

  klass->duplicate_picture =
      GST_DEBUG_FUNCPTR (gst_av1_decoder_duplicate_picture_default);

  g_object_class_install_property (gobject_class, PROP_OPPOINT,
      g_param_spec_int ("oppoint", "Operating Point",
          "Choose an operating point for a scalable stream",
          0, 31, DEFAULT_OPPOINT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
gst_av1_decoder_init (GstAV1Decoder * self)
{
  gst_video_decoder_set_packetized (GST_VIDEO_DECODER (self), TRUE);

  self->priv = gst_av1_decoder_get_instance_private (self);
  self->priv->operating_point = DEFAULT_OPPOINT;
}

static void
gst_av1_decoder_reset (GstAV1Decoder * self)
{
  GstAV1DecoderPrivate *priv = self->priv;

  priv->max_width = 0;
  priv->max_height = 0;
  gst_av1_picture_clear (&priv->current_picture);
  priv->current_frame = NULL;
  priv->profile = GST_AV1_PROFILE_UNDEFINED;

  if (priv->dpb)
    gst_av1_dpb_clear (priv->dpb);
  if (priv->parser)
    gst_av1_parser_reset (priv->parser, FALSE);
}

static gboolean
gst_av1_decoder_start (GstVideoDecoder * decoder)
{
  GstAV1Decoder *self = GST_AV1_DECODER (decoder);
  GstAV1DecoderPrivate *priv = self->priv;

  priv->parser = gst_av1_parser_new ();
  priv->dpb = gst_av1_dpb_new ();

  gst_av1_decoder_reset (self);

  return TRUE;
}

static gboolean
gst_av1_decoder_stop (GstVideoDecoder * decoder)
{
  GstAV1Decoder *self = GST_AV1_DECODER (decoder);
  GstAV1DecoderPrivate *priv = self->priv;

  gst_av1_decoder_reset (self);

  g_clear_pointer (&self->input_state, gst_video_codec_state_unref);
  g_clear_pointer (&priv->parser, gst_av1_parser_free);
  g_clear_pointer (&priv->dpb, gst_av1_dpb_free);

  return TRUE;
}

static gboolean
gst_av1_decoder_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state)
{
  GstAV1Decoder *self = GST_AV1_DECODER (decoder);
  GstAV1DecoderPrivate *priv = self->priv;

  GST_DEBUG_OBJECT (decoder, "Set format");

  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);

  self->input_state = gst_video_codec_state_ref (state);

  priv->max_width = GST_VIDEO_INFO_WIDTH (&state->info);
  priv->max_height = GST_VIDEO_INFO_HEIGHT (&state->info);

  return TRUE;
}

static GstFlowReturn
gst_av1_decoder_finish (GstVideoDecoder * decoder)
{
  GST_DEBUG_OBJECT (decoder, "finish");

  gst_av1_decoder_reset (GST_AV1_DECODER (decoder));

  return GST_FLOW_OK;
}

static gboolean
gst_av1_decoder_flush (GstVideoDecoder * decoder)
{
  GST_DEBUG_OBJECT (decoder, "flush");

  gst_av1_decoder_reset (GST_AV1_DECODER (decoder));

  return TRUE;
}

static GstFlowReturn
gst_av1_decoder_drain (GstVideoDecoder * decoder)
{
  GST_DEBUG_OBJECT (decoder, "drain");

  gst_av1_decoder_reset (GST_AV1_DECODER (decoder));

  return GST_FLOW_OK;
}

static GstAV1Picture *
gst_av1_decoder_duplicate_picture_default (GstAV1Decoder * decoder,
    GstAV1Picture * picture)
{
  GstAV1Picture *new_picture;

  new_picture = gst_av1_picture_new ();

  return new_picture;
}

static void
gst_av1_decoder_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstAV1Decoder *self = GST_AV1_DECODER (object);

  switch (prop_id) {
    case PROP_OPPOINT:
      g_value_set_int (value, self->priv->operating_point);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_av1_decoder_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstAV1Decoder *self = GST_AV1_DECODER (object);

  switch (prop_id) {
    case PROP_OPPOINT:
      self->priv->operating_point = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static const gchar *
get_obu_name (GstAV1OBUType type)
{
  switch (type) {
    case GST_AV1_OBU_SEQUENCE_HEADER:
      return "sequence header";
    case GST_AV1_OBU_TEMPORAL_DELIMITER:
      return "temporal delimiter";
    case GST_AV1_OBU_FRAME_HEADER:
      return "frame header";
    case GST_AV1_OBU_TILE_GROUP:
      return "tile group";
    case GST_AV1_OBU_METADATA:
      return "metadata";
    case GST_AV1_OBU_FRAME:
      return "frame";
    case GST_AV1_OBU_REDUNDANT_FRAME_HEADER:
      return "redundant frame header";
    case GST_AV1_OBU_TILE_LIST:
      return "tile list";
    case GST_AV1_OBU_PADDING:
      return "padding";
    default:
      return "unknown";
  }

  return NULL;
}

static const gchar *
gst_av1_decoder_profile_to_string (GstAV1Profile profile)
{
  switch (profile) {
    case GST_AV1_PROFILE_0:
      return "0";
    case GST_AV1_PROFILE_1:
      return "1";
    case GST_AV1_PROFILE_2:
      return "2";
    default:
      break;
  }

  return NULL;
}

static gboolean
gst_av1_decoder_process_sequence (GstAV1Decoder * self, GstAV1OBU * obu)
{
  GstAV1ParserResult res;
  GstAV1DecoderPrivate *priv = self->priv;
  GstAV1SequenceHeaderOBU seq_header;
  GstAV1SequenceHeaderOBU old_seq_header = { 0, };
  GstAV1DecoderClass *klass = GST_AV1_DECODER_GET_CLASS (self);

  if (priv->parser->seq_header)
    old_seq_header = *priv->parser->seq_header;

  res = gst_av1_parser_parse_sequence_header_obu (priv->parser,
      obu, &seq_header);
  if (res != GST_AV1_PARSER_OK) {
    GST_WARNING_OBJECT (self, "Parsing sequence failed.");
    return FALSE;
  }

  if (!memcmp (&old_seq_header, &seq_header, sizeof (GstAV1SequenceHeaderOBU))) {
    GST_DEBUG_OBJECT (self, "Get same sequence header.");
    return TRUE;
  }

  g_assert (klass->new_sequence);

  GST_DEBUG_OBJECT (self,
      "Sequence updated, profile %s -> %s, max resolution: %dx%d -> %dx%d",
      gst_av1_decoder_profile_to_string (priv->profile),
      gst_av1_decoder_profile_to_string (seq_header.seq_profile),
      priv->max_width, priv->max_height, seq_header.max_frame_width_minus_1 + 1,
      seq_header.max_frame_height_minus_1 + 1);

  if (!klass->new_sequence (self, &seq_header)) {
    GST_ERROR_OBJECT (self, "subclass does not want accept new sequence");
    return FALSE;
  }

  priv->profile = seq_header.seq_profile;
  priv->max_width = seq_header.max_frame_width_minus_1 + 1;
  priv->max_height = seq_header.max_frame_height_minus_1 + 1;
  gst_av1_dpb_clear (priv->dpb);

  return TRUE;
}

static gboolean
gst_av1_decoder_decode_tile_group (GstAV1Decoder * self,
    GstAV1TileGroupOBU * tile_group, GstAV1OBU * obu)
{
  GstAV1DecoderPrivate *priv = self->priv;
  GstAV1DecoderClass *klass = GST_AV1_DECODER_GET_CLASS (self);
  GstAV1Picture *picture = priv->current_picture;
  GstAV1Tile tile;

  if (!picture) {
    GST_ERROR_OBJECT (self, "No picture has created for current frame");
    return FALSE;
  }

  if (picture->frame_hdr.show_existing_frame) {
    GST_ERROR_OBJECT (self, "Current picture is showing the existing frame.");
    return FALSE;
  }

  tile.obu = *obu;
  tile.tile_group = *tile_group;

  g_assert (klass->decode_tile);
  if (!klass->decode_tile (self, picture, &tile)) {
    GST_ERROR_OBJECT (self, "Decode tile error");
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_av1_decoder_decode_frame_header (GstAV1Decoder * self,
    GstAV1FrameHeaderOBU * frame_header)
{
  GstAV1DecoderPrivate *priv = self->priv;
  GstAV1DecoderClass *klass = GST_AV1_DECODER_GET_CLASS (self);
  GstAV1Picture *picture = NULL;

  g_assert (priv->current_frame);

  if (priv->current_picture != NULL) {
    GST_ERROR_OBJECT (self, "Already have picture for current frame");
    return FALSE;
  }

  if (frame_header->show_existing_frame) {
    GstAV1Picture *ref_picture;

    ref_picture = priv->dpb->pic_list[frame_header->frame_to_show_map_idx];
    if (!ref_picture) {
      GST_WARNING_OBJECT (self, "Failed to find the frame index %d to show.",
          frame_header->frame_to_show_map_idx);
      return FALSE;
    }

    if (gst_av1_parser_reference_frame_loading (priv->parser,
            &ref_picture->frame_hdr) != GST_AV1_PARSER_OK) {
      GST_WARNING_OBJECT (self, "load the reference frame failed");
      return FALSE;
    }

    g_assert (klass->duplicate_picture);
    picture = klass->duplicate_picture (self, ref_picture);
    if (!picture) {
      GST_ERROR_OBJECT (self, "subclass didn't provide duplicated picture");
      return FALSE;
    }

    picture->system_frame_number = priv->current_frame->system_frame_number;
    picture->frame_hdr = *frame_header;
    picture->frame_hdr.render_width = ref_picture->frame_hdr.render_width;
    picture->frame_hdr.render_height = ref_picture->frame_hdr.render_height;
    priv->current_picture = picture;
  } else {
    picture = gst_av1_picture_new ();
    picture->frame_hdr = *frame_header;
    picture->display_frame_id = frame_header->display_frame_id;
    picture->show_frame = frame_header->show_frame;
    picture->showable_frame = frame_header->showable_frame;
    picture->apply_grain = frame_header->film_grain_params.apply_grain;
    picture->system_frame_number = priv->current_frame->system_frame_number;

    if (!frame_header->show_frame && !frame_header->showable_frame)
      GST_VIDEO_CODEC_FRAME_FLAG_SET (priv->current_frame,
          GST_VIDEO_CODEC_FRAME_FLAG_DECODE_ONLY);

    if (klass->new_picture) {
      if (!klass->new_picture (self, priv->current_frame, picture)) {
        GST_ERROR_OBJECT (self, "new picture error");
        return FALSE;
      }
    }
    priv->current_picture = picture;

    if (klass->start_picture) {
      if (!klass->start_picture (self, picture, priv->dpb)) {
        GST_ERROR_OBJECT (self, "start picture error");
        return FALSE;
      }
    }
  }

  g_assert (priv->current_picture != NULL);

  return TRUE;
}

static gboolean
gst_av1_decoder_process_frame_header (GstAV1Decoder * self, GstAV1OBU * obu)
{
  GstAV1ParserResult res;
  GstAV1DecoderPrivate *priv = self->priv;
  GstAV1FrameHeaderOBU frame_header;

  res = gst_av1_parser_parse_frame_header_obu (priv->parser, obu,
      &frame_header);
  if (res != GST_AV1_PARSER_OK) {
    GST_WARNING_OBJECT (self, "Parsing frame header failed.");
    return FALSE;
  }

  return gst_av1_decoder_decode_frame_header (self, &frame_header);
}

static gboolean
gst_av1_decoder_process_tile_group (GstAV1Decoder * self, GstAV1OBU * obu)
{
  GstAV1ParserResult res;
  GstAV1DecoderPrivate *priv = self->priv;
  GstAV1TileGroupOBU tile_group;

  res = gst_av1_parser_parse_tile_group_obu (priv->parser, obu, &tile_group);
  if (res != GST_AV1_PARSER_OK) {
    GST_WARNING_OBJECT (self, "Parsing tile group failed.");
    return FALSE;
  }

  return gst_av1_decoder_decode_tile_group (self, &tile_group, obu);
}

static gboolean
gst_av1_decoder_process_frame (GstAV1Decoder * self, GstAV1OBU * obu)
{
  GstAV1ParserResult res;
  GstAV1DecoderPrivate *priv = self->priv;
  GstAV1FrameOBU frame;

  res = gst_av1_parser_parse_frame_obu (priv->parser, obu, &frame);
  if (res != GST_AV1_PARSER_OK) {
    GST_WARNING_OBJECT (self, "Parsing frame failed.");
    return FALSE;
  }

  return gst_av1_decoder_decode_frame_header (self, &frame.frame_header) &&
      gst_av1_decoder_decode_tile_group (self, &frame.tile_group, obu);
}

static gboolean
gst_av1_decoder_temporal_delimiter (GstAV1Decoder * self, GstAV1OBU * obu)
{
  GstAV1DecoderPrivate *priv = self->priv;

  return gst_av1_parser_parse_temporal_delimiter_obu (priv->parser,
      obu) == GST_AV1_PARSER_OK;
}

static gboolean
gst_av1_decoder_decode_one_obu (GstAV1Decoder * self, GstAV1OBU * obu)
{
  gboolean ret = TRUE;

  GST_LOG_OBJECT (self, "Decode obu %s", get_obu_name (obu->obu_type));
  switch (obu->obu_type) {
    case GST_AV1_OBU_SEQUENCE_HEADER:
      ret = gst_av1_decoder_process_sequence (self, obu);
      break;
    case GST_AV1_OBU_FRAME_HEADER:
      ret = gst_av1_decoder_process_frame_header (self, obu);
      break;
    case GST_AV1_OBU_FRAME:
      ret = gst_av1_decoder_process_frame (self, obu);
      break;
    case GST_AV1_OBU_TILE_GROUP:
      ret = gst_av1_decoder_process_tile_group (self, obu);
      break;
    case GST_AV1_OBU_TEMPORAL_DELIMITER:
      ret = gst_av1_decoder_temporal_delimiter (self, obu);
      break;
      /* TODO: may need to handled. */
    case GST_AV1_OBU_METADATA:
    case GST_AV1_OBU_REDUNDANT_FRAME_HEADER:
    case GST_AV1_OBU_TILE_LIST:
    case GST_AV1_OBU_PADDING:
      ret = TRUE;
      break;
    default:
      GST_WARNING_OBJECT (self, "an unrecognized obu type %d", obu->obu_type);
      ret = FALSE;
      break;
  }

  if (!ret)
    GST_WARNING_OBJECT (self, "Failed to handle %s OBU",
        get_obu_name (obu->obu_type));

  return ret;
}

static void
gst_av1_decoder_update_state (GstAV1Decoder * self)
{
  GstAV1DecoderPrivate *priv = self->priv;
  GstAV1Picture *picture = priv->current_picture;
  GstAV1ParserResult res;
  GstAV1FrameHeaderOBU *fh;

  g_assert (picture);
  fh = &picture->frame_hdr;

  /* This is a show_existing_frame case, only update key frame */
  if (fh->show_existing_frame && fh->frame_type != GST_AV1_KEY_FRAME)
    return;

  res = gst_av1_parser_reference_frame_update (priv->parser, fh);
  if (res != GST_AV1_PARSER_OK) {
    GST_ERROR_OBJECT (self, "failed to update the reference.");
    return;
  }

  gst_av1_dpb_add (priv->dpb, gst_av1_picture_ref (picture));
}

static GstFlowReturn
gst_av1_decoder_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  GstAV1Decoder *self = GST_AV1_DECODER (decoder);
  GstAV1DecoderPrivate *priv = self->priv;
  GstAV1DecoderClass *klass = GST_AV1_DECODER_GET_CLASS (self);
  GstBuffer *in_buf = frame->input_buffer;
  GstMapInfo map;
  GstFlowReturn ret = GST_FLOW_OK;
  guint32 total_consumed, consumed;
  GstAV1OBU obu;
  GstAV1ParserResult res;

  GST_LOG_OBJECT (self, "handle frame id %d, buf %" GST_PTR_FORMAT,
      frame->system_frame_number, in_buf);

  priv->current_frame = frame;
  g_assert (!priv->current_picture);

  if (!gst_buffer_map (in_buf, &map, GST_MAP_READ)) {
    priv->current_frame = NULL;
    GST_ERROR_OBJECT (self, "can not map input buffer");
    ret = GST_FLOW_ERROR;
    return ret;
  }

  gst_av1_parser_set_operating_point (priv->parser, priv->operating_point);

  total_consumed = 0;
  while (total_consumed < map.size) {
    res = gst_av1_parser_identify_one_obu (priv->parser,
        map.data + total_consumed, map.size, &obu, &consumed);
    if (res != GST_AV1_PARSER_OK) {
      ret = GST_FLOW_ERROR;
      goto out;
    }

    if (!gst_av1_decoder_decode_one_obu (self, &obu)) {
      ret = GST_FLOW_ERROR;
      goto out;
    }

    total_consumed += consumed;
  }

  if (!priv->current_picture) {
    GST_ERROR_OBJECT (self, "No valid picture after exhaust input frame");
    ret = GST_FLOW_ERROR;
    goto out;
  }

  if (!priv->current_picture->frame_hdr.show_existing_frame) {
    if (klass->end_picture) {
      if (!klass->end_picture (self, priv->current_picture)) {
        ret = GST_FLOW_ERROR;
        GST_ERROR_OBJECT (self, "end picture error");
        goto out;
      }
    }
  }

  gst_av1_decoder_update_state (self);

out:
  gst_buffer_unmap (in_buf, &map);

  if (ret == GST_FLOW_OK) {
    if (priv->current_picture->frame_hdr.show_frame ||
        priv->current_picture->frame_hdr.show_existing_frame) {
      /* Only output one frame with highest spatial id from each TU within
       * the selected operating point, drop other frame(s) with lower spatial id
       */
      if (priv->parser->state.operating_point_idc &&
          obu.header.obu_spatial_id <
          _floor_log2 (priv->parser->state.operating_point_idc >> 8)) {
        gst_av1_picture_unref (priv->current_picture);
        ret = GST_FLOW_OK;
      } else {
        g_assert (klass->output_picture);
        /* transfer ownership of frame and picture */
        ret = klass->output_picture (self, frame, priv->current_picture);
      }
    } else {
      GST_LOG_OBJECT (self, "Decode only picture %p", priv->current_picture);
      GST_VIDEO_CODEC_FRAME_SET_DECODE_ONLY (frame);
      gst_av1_picture_unref (priv->current_picture);
      ret = gst_video_decoder_finish_frame (GST_VIDEO_DECODER (self), frame);
    }
  } else {
    if (priv->current_picture)
      gst_av1_picture_unref (priv->current_picture);

    GST_VIDEO_DECODER_ERROR (decoder, 1, STREAM, DECODE,
        ("Failed to handle the frame %d", frame->system_frame_number),
        NULL, ret);
    gst_video_decoder_drop_frame (decoder, frame);
  }

  priv->current_picture = NULL;
  priv->current_frame = NULL;
  return ret;
}
