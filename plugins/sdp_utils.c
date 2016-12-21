#include <glib.h>
#include <string.h>
#include <stdio.h>
#include "sdp_utils.h"

typedef struct
{
	const gchar *name;
	idilia_codec id;
} codec_name_mapping_t;

codec_name_mapping_t codec_name_mapping[] = 
{
	{ "H264",    IDILIA_CODEC_H264    },
	{ "VP8",     IDILIA_CODEC_VP8     },
	{ "VP9",     IDILIA_CODEC_VP9     },
	{ "opus",    IDILIA_CODEC_OPUS    },
	{ "INVALID", IDILIA_CODEC_INVALID }
};

static gchar * str_replace_once(const gchar * input, const gchar * old_string, const gchar * new_string);
static gint sdp_get_codec_pt_for_type(const gchar * sdp, const gchar * type);
static idilia_codec sdp_pt_to_codec_id(const char * sdp, gint pt);
static idilia_codec sdp_pt_to_codec_id(const char * sdp, gint pt);

const gchar * get_codec_name(idilia_codec codec)
{
	for (guint i = 0; i < sizeof(codec_name_mapping) / sizeof(codec_name_mapping[0]); i++) {
		if (codec == codec_name_mapping[i].id) {
			return codec_name_mapping[i].name;
		}
	}
	
	return "INVALID";
}

idilia_codec sdp_codec_name_to_id(const gchar * name)
{
	for (guint i = 0; i < sizeof(codec_name_mapping) / sizeof(codec_name_mapping[0]); i++) {
		if (!g_strcmp0(codec_name_mapping[i].name, name)) {
			return codec_name_mapping[i].id;
		}
	}

	return IDILIA_CODEC_INVALID;
}

gint sdp_get_codec_pt(const gchar * sdp, idilia_codec codec)
{
	gint pt = -1;
	const gchar * codec_str = get_codec_name(codec);
	gchar * expr_str = g_strdup_printf("a=rtpmap:[0-9]+[ \t]+%s/", codec_str);
	
	GRegex *regex = g_regex_new(expr_str, 0, 0, NULL);
	g_free(expr_str);

	if (regex != NULL) {
		GMatchInfo *matchInfo;
		g_regex_match(regex, sdp, 0, &matchInfo);
	
		if (g_match_info_matches(matchInfo)) {
			gchar *result = g_match_info_fetch(matchInfo, 0);
		
			if (result) {
				gchar * sscanf_str = g_strdup_printf("a=rtpmap:%%d%%*[ \t]%s/", codec_str);
				sscanf(result, sscanf_str, &pt);
				
				g_free(sscanf_str);
				g_free(result);
			}
		}
		g_regex_unref(regex);
	}

	return pt;
}

idilia_codec sdp_get_video_codec(const gchar * sdp)
{
	gint codec_pt = sdp_get_codec_pt_for_type(sdp, "video");
	
	return sdp_pt_to_codec_id(sdp, codec_pt);
}

idilia_codec sdp_get_audio_codec(const gchar * sdp)
{
	gint codec_pt = sdp_get_codec_pt_for_type(sdp, "audio");
	
	return sdp_pt_to_codec_id(sdp, codec_pt);
}

gchar * sdp_set_video_codec(const gchar * sdp_offer, idilia_codec video_codec) {
	
	gchar * sdp_answer = NULL;
	
	gint current_codec_pt = sdp_get_codec_pt_for_type(sdp_offer, "video");
	gint desired_codec_pt = sdp_get_codec_pt(sdp_offer, video_codec);
	
	/* do nothing in case the preferred codec is already selected, or if it does not exist in the SDP */
	if (current_codec_pt == desired_codec_pt || video_codec == IDILIA_CODEC_INVALID) {
		
		return g_strdup(sdp_offer);
		
	} else {
		gchar *result = NULL;		
		GRegex *regex = g_regex_new("m=video[ \t]+[0-9]+[ \t]+UDP/TLS/RTP/SAVPF[ \t]+[0-9]+[ \t]+[0-9]+", 0, 0, NULL);
		gint codec1 = -1, codec2 = -1;
		gint port_video = -1;
		if (regex != NULL) {
			GMatchInfo *matchInfo;
			g_regex_match(regex, sdp_offer, 0, &matchInfo);
			
			if (g_match_info_matches(matchInfo)) {
				result = g_match_info_fetch(matchInfo, 0);
				
				if (result) {					
					sscanf(result,"m=video%*[ \t]%d%*[ \t]UDP/TLS/RTP/SAVPF%*[ \t]%d%*[ \t]%d" ,&port_video,&codec1,&codec2);
					if (codec2 != desired_codec_pt) {
						codec1 = codec2;
					}
					
					gchar *new_line = g_strdup_printf("m=video %d UDP/TLS/RTP/SAVPF %d %d",port_video, desired_codec_pt, codec1);
					
					sdp_answer = str_replace_once(sdp_offer, result, new_line);
					
					g_free(new_line);
					g_free(result);
				}
			}
			else {
				sdp_answer = g_strdup(sdp_offer);
			}
			
			g_regex_unref(regex);
		}
	}
	
	return sdp_answer;
}

static idilia_codec sdp_pt_to_codec_id(const char * sdp, gint pt)
{
	gchar *name = NULL;
	gchar * expr_str = g_strdup_printf("a=rtpmap:%d[ \t]+[a-zA-Z0-9]+", pt);
	GRegex *regex = g_regex_new(expr_str, 0, 0, NULL);
	
	g_free(expr_str);

	if (regex != NULL) {
		GMatchInfo *matchInfo;
		g_regex_match(regex, sdp, 0, &matchInfo);
	
		if (g_match_info_matches(matchInfo))
		{
			gchar *result = g_match_info_fetch(matchInfo, 0);
			
			if (result)
			{
				gchar * sscanf_str = g_strdup_printf("a=rtpmap:%d%%*[ \t]%%s", pt);				
				name = g_malloc(strlen(result));
				sscanf(result, sscanf_str, name);
				g_free(sscanf_str);
				g_free(result);
			}
		}
		g_regex_unref(regex);
	}
	
	idilia_codec codec = sdp_codec_name_to_id(name);
	
	if (name) {
		g_free(name);
	}
	
	return codec;
}

static gchar * str_replace_once(const gchar * input, const gchar * old_string, const gchar * new_string)
{			
	gchar * result = NULL;
	gchar * first_occurence = strstr(input, old_string);
	
	if (first_occurence)
	{
		gchar * prefix = g_strndup(input, first_occurence - input);
		gchar * postfix = first_occurence + strlen(old_string);
		result  = g_strconcat(prefix, new_string, postfix, NULL);
		g_free(prefix);
	}
													
	return result;
}

static gint sdp_get_codec_pt_for_type(const gchar * sdp, const gchar * type)
{
	gchar *result = NULL;
	gchar * expr_str = g_strdup_printf("m=%s[ \t]+[0-9]+[ \t]+UDP/TLS/RTP/SAVPF[ \t]+[0-9]+", type);
	GRegex *regex = g_regex_new(expr_str, 0, 0, NULL);
	gint codec_pt = -1;
	
	g_free(expr_str);
	
	if (regex != NULL) {
		GMatchInfo *matchInfo;
		g_regex_match(regex, sdp, 0, &matchInfo);
	
		if (g_match_info_matches(matchInfo)) {
			result = g_match_info_fetch(matchInfo, 0);
				
			if (result) {							
				gchar * sscanf_str = g_strdup_printf("m=%s%%*[ \t]%%*d%%*[ \t]UDP/TLS/RTP/SAVPF%%*[ \t]%%d", type);	 
				sscanf(result, sscanf_str, &codec_pt);
				 
				g_free(sscanf_str);
				g_free(result);
			}
		}
			
		g_regex_unref(regex);
	}
	
	return codec_pt;
}
