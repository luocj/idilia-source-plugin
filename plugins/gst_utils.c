#include "debug.h"
#include "gst_utils.h"
#include "idilia_source_common.h"
#include "audio_video_defines.h"


#ifdef PLI_WORKAROUND
static gboolean request_key_frame_cb(gpointer data);
static gboolean request_key_frame_if_not_playing_cb(gpointer data);
#endif
static void client_play_request_cb(GstRTSPClient  *gstrtspclient, GstRTSPContext *rtspcontext, gpointer data);
static GstSDPMessage * create_sdp(GstRTSPClient * client, GstRTSPMedia * media);
static const gchar * janus_source_get_udpsrc_name(int stream, int type);

/* External declarations (janus.h) */
extern gchar *janus_get_local_ip(void);

static GstSDPMessage *
create_sdp(GstRTSPClient * client, GstRTSPMedia * media)
{
	GstSDPMessage *sdp;
	GstSDPInfo info;

	guint64 session_id_tmp;
	gchar session_id[21];
	const char * server_ip = janus_get_local_ip();
	const gchar *proto = "IP4"; //todo: support IPV6

	gst_sdp_message_new(&sdp);

	/* some standard things first */
	gst_sdp_message_set_version(sdp, "0");

	session_id_tmp = (((guint64)g_random_int()) << 32) | g_random_int();
	g_snprintf(session_id,
		sizeof(session_id),
		"%" G_GUINT64_FORMAT,
		session_id_tmp);

	gst_sdp_message_set_origin(sdp, "-", session_id, "1", "IN", proto, server_ip);

	gst_sdp_message_set_session_name(sdp, "Idilia source session");
	gst_sdp_message_set_information(sdp, "rtsp-server");
	gst_sdp_message_add_time(sdp, "0", "0", NULL);
	gst_sdp_message_add_attribute(sdp, "tool", "GStreamer");
	gst_sdp_message_add_attribute(sdp, "type", "broadcast");
	gst_sdp_message_add_attribute(sdp, "control", "*");

	info.is_ipv6 = !! g_strcmp0(proto, "IP4");
	info.server_ip = server_ip;

	/* create an SDP for the media object */
	if (!gst_rtsp_media_setup_sdp(media, sdp, &info))
		goto no_sdp;

	GstSDPMedia * sdpmedia = (GstSDPMedia *)gst_sdp_message_get_media(sdp, 0);
	g_assert(sdpmedia);

	gst_sdp_media_add_attribute(sdpmedia, "rtcp-fb", "96 nack");
	gst_sdp_media_add_attribute(sdpmedia, "rtcp-fb", "96 nack pli");

	return sdp;

	/* ERRORS */
no_sdp:
	{
		GST_ERROR("client %p: could not create SDP", client);
		gst_sdp_message_free(sdp);
		return NULL;
	}
}

//todo: fix
static const gchar * janus_source_get_udpsrc_name(int stream, int type) { 
	switch(stream)
	{
		case JANUS_SOURCE_STREAM_VIDEO :
			switch (type)
			{
			case JANUS_SOURCE_SOCKET_RTP_SRV:
				return "udpsrc_rtp_video";
			case JANUS_SOURCE_SOCKET_RTCP_RCV_SRV:
				return "udpsrc_rtcp_receive_video";
			default:
				break;
			}

			break ;

		case JANUS_SOURCE_STREAM_AUDIO :
			switch (type)
			{
			case JANUS_SOURCE_SOCKET_RTP_SRV:
				return "udpsrc_rtp_audio";
			case JANUS_SOURCE_SOCKET_RTCP_RCV_SRV:
				return "udpsrc_rtcp_receive_audio";
			default:
				break;
			}
			break ;
		default:
			break;
	 }
	return NULL;
}

static void rtsp_media_target_state_cb(GstRTSPMedia *gstrtspmedia, gint state, gpointer data)
{
	JANUS_LOG(LOG_INFO, "rtsp_media_target_state_cb: %d\n", (GstState)state);
	janus_source_session * session = (janus_source_session *)data;
	
	if (!session) {
		JANUS_LOG(LOG_ERR, "rtsp_media_target_state_cb: session is NULL\n");
		return;
	}

#ifdef PLI_WORKAROUND

	g_atomic_int_set(&session->rtsp_state, state);
	
	if (state == GST_STATE_PAUSED) {
		janus_source_request_keyframe(data);
		/* ensure after 2s if keyframe was really obtained and pipeline switched into PLAYING */
		g_timeout_add(2000, request_key_frame_if_not_playing_cb, data); 
	}
#endif

	if (state == GST_STATE_PAUSED) {
		GstElement * bin = NULL;
		GSocket * socket = NULL;

		bin = gst_rtsp_media_get_element(gstrtspmedia);
		g_assert(bin);
		
		for (int stream = 0; stream < JANUS_SOURCE_STREAM_MAX; stream++)
		{
			GstElement * udp_src_rtp = gst_bin_get_by_name(GST_BIN(bin), janus_source_get_udpsrc_name(stream, JANUS_SOURCE_SOCKET_RTP_SRV));
			g_assert(udp_src_rtp);
		
			socket = session->socket[stream][JANUS_SOURCE_SOCKET_RTP_SRV].socket;
			g_assert(socket);
			g_object_set(udp_src_rtp, "socket", socket, NULL);
			g_object_unref(udp_src_rtp);
		
			GstElement * udpsrc_rtcp_receive = gst_bin_get_by_name(GST_BIN(bin), janus_source_get_udpsrc_name(stream, JANUS_SOURCE_SOCKET_RTCP_RCV_SRV));
			g_assert(udpsrc_rtcp_receive);
		
			socket = session->socket[stream][JANUS_SOURCE_SOCKET_RTCP_RCV_SRV].socket;
			g_assert(socket);
			g_object_set(udpsrc_rtcp_receive, "socket", socket, NULL);
			g_object_unref(udpsrc_rtcp_receive);
		}
		g_object_unref(bin);
	}
}

void media_configure_cb(GstRTSPMediaFactory * factory, GstRTSPMedia * media, gpointer data)
{
	JANUS_LOG(LOG_INFO, "media_configure callback\n") ;
	g_signal_connect(media, "target-state", (GCallback)rtsp_media_target_state_cb, data);
}

static void
client_play_request_cb(GstRTSPClient  *gstrtspclient,
	GstRTSPContext *rtspcontext,
	gpointer        data)
{
	JANUS_LOG(LOG_INFO, "client_play_request_cb\n");
#ifdef PLI_WORKAROUND
	//give clients some time to start collecting data
	g_timeout_add(1000, request_key_frame_cb, data) ;
#endif
}


void
client_connected_cb(GstRTSPServer *gstrtspserver,
	GstRTSPClient *gstrtspclient,
	gpointer       data)
{
	GstRTSPClientClass *klass = GST_RTSP_CLIENT_GET_CLASS(gstrtspclient);
	klass->create_sdp = create_sdp;
	JANUS_LOG(LOG_INFO, "New client connected\n");		
	g_signal_connect(gstrtspclient, "play-request", (GCallback)client_play_request_cb, data);
}

GstRTSPFilterResult
janus_source_close_rtsp_sessions(GstRTSPSessionPool *pool, GstRTSPSession *session, gpointer data) { 
	JANUS_LOG(LOG_INFO, "Removing RTSP session: %s\n", gst_rtsp_session_get_sessionid(session));
	return GST_RTSP_FILTER_REMOVE;	
}


gchar * janus_source_create_launch_pipe(janus_source_session * session) {
	gchar * launch_pipe = NULL;
	gchar * launch_pipe_video = NULL;
	gchar * launch_pipe_audio = NULL;
	
	for (int stream = 0; stream < JANUS_SOURCE_STREAM_MAX; stream++)
	{
		switch (session->codec[stream])
		{
		case IDILIA_CODEC_VP8:
			launch_pipe_video = g_strdup_printf(PIPE_VIDEO_VP8,
				session->codec_pt[stream],
				janus_source_get_udpsrc_name(stream, JANUS_SOURCE_SOCKET_RTP_SRV),
				janus_source_get_udpsrc_name(stream, JANUS_SOURCE_SOCKET_RTCP_RCV_SRV),
				session->socket[stream][JANUS_SOURCE_SOCKET_RTCP_SND_SRV].port);
			break;
		case IDILIA_CODEC_VP9:
			launch_pipe_video = g_strdup_printf(PIPE_VIDEO_VP9,
				session->codec_pt[stream],
				janus_source_get_udpsrc_name(stream, JANUS_SOURCE_SOCKET_RTP_SRV),
				janus_source_get_udpsrc_name(stream, JANUS_SOURCE_SOCKET_RTCP_RCV_SRV),
				session->socket[stream][JANUS_SOURCE_SOCKET_RTCP_SND_SRV].port);
			break;
		case IDILIA_CODEC_H264:
			launch_pipe_video = g_strdup_printf(PIPE_VIDEO_H264,
				session->codec_pt[stream],
				janus_source_get_udpsrc_name(stream, JANUS_SOURCE_SOCKET_RTP_SRV),
				janus_source_get_udpsrc_name(stream, JANUS_SOURCE_SOCKET_RTCP_RCV_SRV),
				session->socket[stream][JANUS_SOURCE_SOCKET_RTCP_SND_SRV].port);
			break;
		case IDILIA_CODEC_OPUS:
			launch_pipe_audio = g_strdup_printf(PIPE_AUDIO_OPUS,
				janus_source_get_udpsrc_name(stream, JANUS_SOURCE_SOCKET_RTP_SRV),
				session->codec_pt[stream],
				janus_source_get_udpsrc_name(stream, JANUS_SOURCE_SOCKET_RTCP_RCV_SRV),
				session->socket[stream][JANUS_SOURCE_SOCKET_RTCP_SND_SRV].port);
			break;
		default: 
			break;
		}
	}

	if (session->codec[JANUS_SOURCE_STREAM_VIDEO] != IDILIA_CODEC_INVALID && session->codec[JANUS_SOURCE_STREAM_AUDIO] != IDILIA_CODEC_INVALID) {
		launch_pipe = g_strdup_printf("( %s name=pay0  %s name=pay1 )", launch_pipe_video, launch_pipe_audio);
	}
	else if (session->codec[JANUS_SOURCE_STREAM_VIDEO] != IDILIA_CODEC_INVALID && session->codec[JANUS_SOURCE_STREAM_AUDIO] == IDILIA_CODEC_INVALID) {
		launch_pipe = g_strdup_printf("( %s name=pay0 )", launch_pipe_video);
	}
	else if (session->codec[JANUS_SOURCE_STREAM_VIDEO] == IDILIA_CODEC_INVALID && session->codec[JANUS_SOURCE_STREAM_AUDIO] != IDILIA_CODEC_INVALID) {
		launch_pipe = g_strdup_printf("( %s name=pay0 )", launch_pipe_audio);
	}

	if (launch_pipe_video) {
		g_free(launch_pipe_video);
	}

	if (launch_pipe_audio) {
		g_free(launch_pipe_audio);
	}

	return launch_pipe;
}

#ifdef PLI_WORKAROUND


static gboolean
request_key_frame_cb(gpointer data)
{
	janus_source_request_keyframe((janus_source_session *)data);
	return FALSE;
}

gboolean
request_key_frame_periodic_cb(gpointer data)
{
	janus_source_request_keyframe((janus_source_session *)data);
	return TRUE;
}

static gboolean
request_key_frame_if_not_playing_cb(gpointer data)
{
	janus_source_session * session = (janus_source_session *)data;

	if (!session) {
		JANUS_LOG(LOG_ERR, "keyframe_once_cb: session is NULL\n");
		return FALSE;
	}

	if (g_atomic_int_get(&session->hangingup) || session->destroyed) {
		JANUS_LOG(LOG_VERB, "Keyframe generation event while plugin or session is stopping\n");
		return FALSE;
	}

	if (g_atomic_int_get(&session->rtsp_state) == GST_STATE_PAUSED) {
		JANUS_LOG(LOG_INFO, "State is GST_STATE_PAUSED; sendig pli and schedulling check\n");
		janus_source_request_keyframe(data);
		/* call this callback again, as we are still in PAUSED state */
		return TRUE;
	}

	return FALSE;
}
#endif
