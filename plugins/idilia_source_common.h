#pragma once

#include <curl/curl.h>
#include "socket_utils.h"
#include "sdp_utils.h"
#include "plugin.h"
#include "rtsp_server.h"
#include "pipeline_callback_data.h"

#define USE_REGISTRY_SERVICE

typedef struct janus_source_session {
	janus_plugin_session *handle;
	gboolean audio_active;
	gboolean video_active;
	uint64_t bitrate;
	guint16 slowlink_count;
	volatile gint hangingup;
	gint64 destroyed;	/* Time at which this session was marked as destroyed */
	gchar * db_entry_session_id;
	gchar * rtsp_url;
	gchar * id; /* stream id */
	CURL *curl_handle;	
	gchar *status_service_url;
	gchar *keepalive_service_url;
	const gchar *pid; 
	idilia_codec codec[JANUS_SOURCE_STREAM_MAX];
	gint codec_pt[JANUS_SOURCE_STREAM_MAX];
    GHashTable * sockets;
	pipeline_callback_data_t * callback_data;
} janus_source_session;


/* idilia_source.c */
extern gboolean janus_source_send_rtcp_src_received(GSocket *socket, GIOCondition condition, janus_source_rtcp_cbk_data * data);
extern const gchar *janus_source_get_rtsp_ip(void);
extern void janus_source_hangup_media(janus_plugin_session *handle);
extern void janus_source_send_id_error(janus_plugin_session *handle); 
extern janus_source_rtsp_server_data *rtsp_server_data;
