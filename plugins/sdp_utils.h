#pragma once

#include <glib.h>
#include "sdp_utils.h"

typedef enum
{
	IDILIA_CODEC_OPUS = 0,
	IDILIA_CODEC_VP8,
	IDILIA_CODEC_VP9,
	IDILIA_CODEC_H264,
	IDILIA_CODEC_MAX,
	IDILIA_CODEC_INVALID = -1
} idilia_codec;

gint sdp_get_codec_pt(const gchar * sdp, idilia_codec codec);
idilia_codec sdp_get_video_codec(const gchar * sdp);
idilia_codec sdp_get_audio_codec(const gchar * sdp);
gchar * sdp_set_video_codec(const gchar * sdp_offer, idilia_codec video_codec);
const gchar * get_codec_name(idilia_codec codec);
idilia_codec sdp_codec_name_to_id(const gchar * name);
