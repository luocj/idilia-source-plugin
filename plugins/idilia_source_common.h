#pragma once

#include <curl/curl.h>
#include "socket_utils.h"
#include "sdp_utils.h"
#include "plugin.h"
#include "rtsp_server.h"

//#define PLI_WORKAROUND

enum
{
	JANUS_SOURCE_STREAM_VIDEO = 0,
	JANUS_SOURCE_STREAM_AUDIO,
	JANUS_SOURCE_STREAM_MAX
};

enum
{
	JANUS_SOURCE_SOCKET_RTP_SRV = 0,
	JANUS_SOURCE_SOCKET_RTP_CLI,
	JANUS_SOURCE_SOCKET_RTCP_RCV_SRV,
	JANUS_SOURCE_SOCKET_RTCP_RCV_CLI,
	JANUS_SOURCE_SOCKET_RTCP_SND_SRV,
	JANUS_SOURCE_SOCKET_MAX
};

typedef struct rtcp_callback_data
{
	gpointer * session;
	gboolean is_video;
} janus_source_rtcp_cbk_data;

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
#ifdef PLI_WORKAROUND
	gint periodic_pli;
	GstState rtsp_state;
#endif
	janus_source_socket socket[JANUS_SOURCE_STREAM_MAX][JANUS_SOURCE_SOCKET_MAX];
	janus_source_rtcp_cbk_data rtcp_cbk_data[JANUS_SOURCE_STREAM_MAX];
	idilia_codec codec[JANUS_SOURCE_STREAM_MAX];
	gint codec_pt[JANUS_SOURCE_STREAM_MAX];
} janus_source_session;

#ifdef PLI_WORKAROUND
void janus_source_request_keyframe(janus_source_session *session);
#endif


/* idilia_source.c */
extern gboolean janus_source_send_rtcp_src_received(GSocket *socket, GIOCondition condition, janus_source_rtcp_cbk_data * data);
extern const gchar *janus_source_get_rtsp_ip(void);
extern janus_source_rtsp_server_data *rtsp_server_data;