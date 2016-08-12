#include "debug.h"
#include "gst_utils.h"
#include "idilia_source_common.h"
#include "audio_video_defines.h"
#include "node_service_access.h"
#include "rtsp_server.h"


static void client_play_request_cb(GstRTSPClient  *gstrtspclient, GstRTSPContext *rtspcontext, gpointer data);
static GstSDPMessage * create_sdp(GstRTSPClient * client, GstRTSPMedia * media);
static const gchar * janus_source_get_udpsrc_name(int stream, int type);
static gchar * janus_source_create_launch_pipe(janus_source_session * session);
static void media_configure_cb(GstRTSPMediaFactory * factory, GstRTSPMedia * media, gpointer data);
static void client_connected_cb(GstRTSPServer *gstrtspserver, GstRTSPClient *gstrtspclient, gpointer data);
static gchar *janus_source_create_json_request(gchar *request);
static gboolean new_session = FALSE; 


static GstSDPMessage *
create_sdp(GstRTSPClient * client, GstRTSPMedia * media)
{
	GstSDPMessage *sdp;
	GstSDPInfo info;

	guint64 session_id_tmp;
	gchar session_id[21];
	const gchar * server_ip = janus_source_get_rtsp_ip();
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

	gst_sdp_media_add_attribute(sdpmedia, "rtcp-fb", "96 ccm fir");
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

static const gchar * janus_source_get_udpsrc_name(int stream, int type) { 
	switch(stream)
	{
		case JANUS_SOURCE_STREAM_VIDEO:
			switch (type)
			{
			case JANUS_SOURCE_SOCKET_RTP_SRV:
				return "udpsrc_rtp_video";
			case JANUS_SOURCE_SOCKET_RTCP_RCV_SRV:
				return "udpsrc_rtcp_rcv_video";
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
				return "udpsrc_rtcp_rcv_audio";
			default:
				break;
			}
			break ;
		default:
			break;
	 }
	
	JANUS_LOG(LOG_ERR, "Error, not implemented!");
	
	return NULL;
}

static void rtsp_new_state_cb(GstRTSPMedia *gstrtspmedia, gint state, gpointer data) {	

	janus_source_session * session = (janus_source_session *)data;
		 
	if (!session) {
		JANUS_LOG(LOG_ERR, "rtsp_new_state_cb: session is NULL\n");
		return;
	}else {
		JANUS_LOG(LOG_VERB,"rtsp_new_state_cb  %s\n",gst_element_state_get_name((GstState)state));		
		g_atomic_int_set(&session->rtsp_session_state,state);
	}
}

static void rtsp_removed_stream_cb(GstRTSPMedia *gstrtspmedia, gint state, gpointer data) {
	JANUS_LOG(LOG_VERB, "***rtsp_removed_stream_cb: %d \n", (GstState)state);
}

static void rtsp_media_target_state_cb(GstRTSPMedia *gstrtspmedia, gint state, gpointer data)
{
	JANUS_LOG(LOG_VERB, "rtsp_media_target_state_cb: %s\n", gst_element_state_get_name((GstState)state));

	janus_source_session * session = (janus_source_session *)data;
	
	 
	if (!session) {
		JANUS_LOG(LOG_ERR, "rtsp_media_target_state_cb: session is NULL\n");
		return;
	}
	
	if (state == GST_STATE_PAUSED && session->rtsp_session_state == GST_STATE_PAUSED && new_session == TRUE ) {
				
		GstElement * bin = NULL;
		GSocket * socket = NULL;
		bin = gst_rtsp_media_get_element(gstrtspmedia);
		g_assert(bin);
		
		for (int stream = 0; stream < JANUS_SOURCE_STREAM_MAX; stream++)
		{
			GstElement * udp_src_rtp = gst_bin_get_by_name(GST_BIN(bin), janus_source_get_udpsrc_name(stream, JANUS_SOURCE_SOCKET_RTP_SRV));			

			g_assert(udp_src_rtp);
		
			socket = session->socket[stream][JANUS_SOURCE_SOCKET_RTP_SRV].socket;			
			if(socket) {
				g_assert(socket);
				g_object_set(udp_src_rtp, "socket", socket, NULL);
				g_object_set(udp_src_rtp, "close-socket", FALSE, NULL);
				g_object_unref(udp_src_rtp);
			}
		
			GstElement * udpsrc_rtcp_receive = gst_bin_get_by_name(GST_BIN(bin), janus_source_get_udpsrc_name(stream, JANUS_SOURCE_SOCKET_RTCP_RCV_SRV));
			
			g_assert(udpsrc_rtcp_receive);
		
			socket = session->socket[stream][JANUS_SOURCE_SOCKET_RTCP_RCV_SRV].socket;
			if(socket) {
				g_assert(socket);
				g_object_set(udpsrc_rtcp_receive, "socket", socket, NULL);
				g_object_set(udpsrc_rtcp_receive, "close-socket", FALSE, NULL);
				g_object_unref(udpsrc_rtcp_receive);					
			}			
		}

		g_object_unref(bin);
		 
	}
		
}

static void media_configure_cb(GstRTSPMediaFactory * factory, GstRTSPMedia * media, gpointer data)
{
	JANUS_LOG(LOG_VERB, "media_configure callback\n") ;
	janus_source_session * session = (janus_source_session *)data;
		 
	if (!session) {
		JANUS_LOG(LOG_ERR, "media_configure_cb: session is NULL\n");
		return;
	}
	else {
		g_atomic_int_set(&session->rtsp_session_state,GST_STATE_PAUSED);
	}
	
	g_signal_connect(media, "target-state", (GCallback)rtsp_media_target_state_cb, data);
	g_signal_connect(media, "new-state", (GCallback)rtsp_new_state_cb, data);
	g_signal_connect(media, "removed-stream", (GCallback)rtsp_removed_stream_cb, data);
}

static void
client_play_request_cb(GstRTSPClient  *gstrtspclient,
	GstRTSPContext *rtspcontext,
	gpointer        data)
{
	JANUS_LOG(LOG_VERB, "client_play_request_cb\n");
}

static void
client_new_session_cb(GstRTSPClient  *gstrtspclient,
	GstRTSPContext *rtspcontext,
	gpointer        data)
{
	JANUS_LOG(LOG_VERB, "client_new_session_cb\n");	
}

static void
client_pause_request_cb(GstRTSPClient  *gstrtspclient,
	GstRTSPContext *rtspcontext,
	gpointer        data)
{
	JANUS_LOG(LOG_VERB, "client_pause_request_cb\n");	
	new_session = FALSE;
}

static void
client_setup_request_cb(GstRTSPClient  *gstrtspclient,
	GstRTSPContext *rtspcontext,
	gpointer        data)
{
	JANUS_LOG(LOG_VERB, "client_setup_request_cb\n");	
}


static void
client_connected_cb(GstRTSPServer *gstrtspserver,
	GstRTSPClient *gstrtspclient,
	gpointer       data)
{
	JANUS_LOG(LOG_VERB, "New client connected\n");	
	GstRTSPClientClass *klass = GST_RTSP_CLIENT_GET_CLASS(gstrtspclient);
	klass->create_sdp = create_sdp;
	new_session = TRUE;		
	g_signal_connect(gstrtspclient, "play-request", (GCallback)client_play_request_cb, data);
	g_signal_connect(gstrtspclient, "new-session",(GCallback)client_new_session_cb, data);
	g_signal_connect(gstrtspclient, "pause-request",(GCallback)client_pause_request_cb, data);
	g_signal_connect(gstrtspclient, "setup-request",(GCallback)client_setup_request_cb, data);
}


static gchar * janus_source_create_launch_pipe(janus_source_session * session) {
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
				session->codec_pt[stream],
				janus_source_get_udpsrc_name(stream, JANUS_SOURCE_SOCKET_RTP_SRV),
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

void janus_rtsp_handle_client_callback(gpointer data) {	
	
	janus_source_session *session = (janus_source_session*)(data); 
	
	if (!session) {
		JANUS_LOG(LOG_ERR, "Session is NULL\n");
		return;
	}

	if (g_atomic_int_get(&session->hangingup) || session->destroyed) {
		JANUS_LOG(LOG_INFO, "Session is being destroyed\n");		 
	}

	for (int i = 0; i < JANUS_SOURCE_STREAM_MAX; i++)
	{
		for (int j = 0; j < JANUS_SOURCE_SOCKET_MAX; j++)
			JANUS_LOG(LOG_VERB, "UDP port[%d][%d]: %d\n", i, j, session->socket[i][j].port);
	}

	const gchar * rtsp_ip = janus_source_get_rtsp_ip();

	GstRTSPMediaFactory *factory;	
	gchar * launch_pipe = janus_source_create_launch_pipe(session);
	factory = janus_source_rtsp_factory(rtsp_server_data, rtsp_ip, launch_pipe);
	g_free(launch_pipe);

	for (int stream = 0; stream < JANUS_SOURCE_STREAM_MAX; stream++)
	{
		session->rtcp_cbk_data[stream].session = (gpointer)session;
		session->rtcp_cbk_data[stream].is_video = (stream == JANUS_SOURCE_STREAM_VIDEO);

		socket_utils_attach_callback(&session->socket[stream][JANUS_SOURCE_SOCKET_RTCP_SND_SRV],
			(GSourceFunc)janus_source_send_rtcp_src_received,
			(gpointer)&session->rtcp_cbk_data[stream]);
	}

	int rtsp_port = 0;
	gchar * uri = g_strdup_printf("/%s",session->id);
	rtsp_port = janus_source_rtsp_server_port(rtsp_server_data);

	session->rtsp_url = g_strdup_printf("rtsp://%s:%d%s", rtsp_ip, rtsp_port, uri);
	
	gchar *http_request_data = janus_source_create_json_request(session->rtsp_url);
	const gchar *HTTP_POST = "POST";
	json_t *db_id_json_object = NULL;

	gboolean retCode = curl_request(session->curl_handle, session->status_service_url, http_request_data, HTTP_POST, &db_id_json_object);
	if (retCode != TRUE) {
		JANUS_LOG(LOG_ERR, "Could not send the request to the server\n");		
	}
	else {
		if (!json_is_object(db_id_json_object)) {
			JANUS_LOG(LOG_ERR, "Not valid json object.\n");				
		}	
		else
		{			
			const gchar *DUPLICATE_FIELD_ERR_CODE = "11000";
			const gchar *DB_ERROR_CODE_FIELD = "code";

			gint code_err = json_integer_value(json_object_get(db_id_json_object,DB_ERROR_CODE_FIELD));

			if(code_err != 0){
            	gchar *code_error_string = g_strdup_printf("%d",code_err);
				if(0 == g_strcmp0(DUPLICATE_FIELD_ERR_CODE,code_error_string)){
					JANUS_LOG(LOG_ERR, "The mountpoint %s already exist in the system\n",uri);
					janus_source_hangup_media(session->handle);		
					janus_source_send_id_error(session->handle);							
				} 
				g_free(code_error_string);
			}
			else {
				const gchar *MEDIA_CONFIGURE_CB = "media-configure";
				const gchar *CLIENT_CONNECTED_CB = "client-connected";
				const gchar *DB_FIELD_ID = "_id";	

				g_signal_connect(factory, MEDIA_CONFIGURE_CB, (GCallback)media_configure_cb, (gpointer)session);

				g_signal_connect(rtsp_server_data->rtsp_server, CLIENT_CONNECTED_CB, (GCallback)client_connected_cb, (gpointer)session);
					
				janus_source_rtsp_mountpoint(rtsp_server_data, factory, uri);
				
				session->db_entry_session_id = (gchar *) g_strdup(json_string_value(json_object_get(db_id_json_object,DB_FIELD_ID)));
				JANUS_LOG(LOG_INFO, "Stream ready at %s\n", session->rtsp_url);
			}
		}
		json_decref(db_id_json_object);
	}

	g_free(http_request_data);
	g_free(uri);
}


gchar *janus_source_create_json_request(gchar *request)
{
	json_t *object = json_object();
	const gchar *URI = "uri";
	const gchar *REGEX_PATTERN = "\\/";
	const gchar *ID_JSON_FIELD = "id";

    json_object_set_new(object, URI, json_string(request)); 

    gchar **result;
	    
    result = g_regex_split_simple (REGEX_PATTERN,request, 0, 0); 
    if (result != NULL) {
		json_object_set_new(object, ID_JSON_FIELD, json_string(result[g_strv_length(result)-1]));
		g_strfreev (result);
    }

    gchar *request_str = json_dumps(object, JSON_PRESERVE_ORDER);
	 
    json_decref(object);
	return request_str;
}