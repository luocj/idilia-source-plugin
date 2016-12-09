#include "debug.h"
#include "gst_utils.h"
#include "idilia_source_common.h"
#include "audio_video_defines.h"
#include "node_service_access.h"
#include "rtsp_server.h"
#include "rtsp_clients_utils.h"
#include "socket_names.h"

static GstSDPMessage * create_sdp(GstRTSPClient * client, GstRTSPMedia * media);
static gchar * janus_source_create_launch_pipe(janus_source_session * session, pipeline_callback_data_t * data);
static void media_configure_cb(GstRTSPMediaFactory * factory, GstRTSPMedia * media, pipeline_callback_data_t * data);
static void client_connected_cb(GstRTSPServer *gstrtspserver, GstRTSPClient *gstrtspclient, pipeline_callback_data_t * data);
static gchar *janus_source_create_json_request(gchar *request);


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

int close_and_destroy_sockets(gpointer key, janus_source_socket * sck, gpointer user_data)
{
	if (sck) {
		JANUS_LOG(LOG_VERB, "Closing socket %p\n", sck);
		socket_utils_close_socket(sck);
		g_free(sck);
	}

	return TRUE;
}

void pipeline_callback_data_destroy(pipeline_callback_data_t * data) {
	g_assert(data);
	JANUS_LOG(LOG_INFO, "Freeing callback data for session: %s\n", data->id);
	g_hash_table_foreach_remove(data->sockets, (GHRFunc)close_and_destroy_sockets, NULL);
	g_hash_table_destroy(data->sockets);
	g_free(data->id);
	g_free(data->rtsp_url);
	g_free(data);
}

static void set_custom_socket(GHashTable * sockets, GstElement *bin, const gchar * socket_name) {
	g_assert(sockets);
	
	janus_source_socket *sck = g_hash_table_lookup(sockets, socket_name);
	GstElement * udp_src = gst_bin_get_by_name(GST_BIN(bin), socket_name);			

	if (!sck || !udp_src || !sck->socket) {
		JANUS_LOG(LOG_FATAL, "Invalid input objects: %p, %p\n", sck, udp_src);
		return;
	}

	g_assert(G_IS_SOCKET(sck->socket));
	g_object_set(udp_src, "socket", sck->socket, NULL);
	g_object_set(udp_src, "close-socket", FALSE, NULL);
	g_object_unref(udp_src);
}


static void rtsp_media_target_state_cb(GstRTSPMedia *gstrtspmedia, gint state, pipeline_callback_data_t * data)
{
	JANUS_LOG(LOG_INFO, "rtsp_media_target_state_cb: %s\n", gst_element_state_get_name((GstState)state));

	if (!data) {
		JANUS_LOG(LOG_ERR, "Calback data is null\n");
		return;
	}
	
	if (state == GST_STATE_PAUSED) {
		JANUS_LOG(LOG_INFO, "Setting custom sockets\n");		
		GstElement * bin = gst_rtsp_media_get_element(gstrtspmedia);
		g_assert(bin);
		
		set_custom_socket(data->sockets, bin, SOCKET_VIDEO_RTP_SRV);
		set_custom_socket(data->sockets, bin, SOCKET_VIDEO_RTCP_RCV_SRV);

		set_custom_socket(data->sockets, bin, SOCKET_AUDIO_RTP_SRV);
		set_custom_socket(data->sockets, bin, SOCKET_AUDIO_RTCP_RCV_SRV);

		g_object_unref(bin);

		if (data->id_rtsp_media_target_state_cb > 0) {
			JANUS_LOG(LOG_INFO, "Disconnecting signal rtsp_media_target_state_cb: %lu\n", data->id_rtsp_media_target_state_cb);
			g_signal_handler_disconnect(gstrtspmedia, data->id_rtsp_media_target_state_cb);
			data->id_rtsp_media_target_state_cb = 0;
		}
	}		
}

static void media_configure_cb(GstRTSPMediaFactory * factory, GstRTSPMedia * media, pipeline_callback_data_t * data)
{
	JANUS_LOG(LOG_INFO, "media_configure callback\n") ;
	 
	if (!data) {
		JANUS_LOG(LOG_ERR, "Calback data is null\n");
		return;
	}

	data->id_rtsp_media_target_state_cb = g_signal_connect(media, "target-state", (GCallback)rtsp_media_target_state_cb, data);
}


static void
client_pause_request_cb(GstRTSPClient  *gstrtspclient,
	GstRTSPContext *rtspcontext,
	pipeline_callback_data_t * data)
{
	JANUS_LOG(LOG_INFO, "client_pause_request_cb\n");	

	if (!data) {
		JANUS_LOG(LOG_ERR, "Calback data is NULL\n");
		return;
	}

	rtsp_clients_list_remove(&data->clients_list, &data->clients_mutex, g_object_ref(gstrtspclient));
}

static void
client_setup_request_cb(GstRTSPClient  *gstrtspclient,
	GstRTSPContext *rtspcontext,
	pipeline_callback_data_t * data)
{
	JANUS_LOG(LOG_INFO, "client_setup_request_cb\n");

	if (!data) {
		JANUS_LOG(LOG_ERR, "Calback data is NULL\n");
		return;
	}

	rtsp_clients_list_add(&data->clients_list, &data->clients_mutex, g_object_ref(gstrtspclient));
}


static void
client_connected_cb(GstRTSPServer *gstrtspserver,
	GstRTSPClient *gstrtspclient,
	pipeline_callback_data_t * data)
{
	JANUS_LOG(LOG_INFO, "New client connected\n");	

	if (!data) {
		JANUS_LOG(LOG_ERR, "Calback data is null\n");
		return;
	}

	GstRTSPClientClass *klass = GST_RTSP_CLIENT_GET_CLASS(gstrtspclient);
	klass->create_sdp = create_sdp;	
	g_signal_connect(gstrtspclient, "pause-request",(GCallback)client_pause_request_cb, data);
	g_signal_connect(gstrtspclient, "setup-request",(GCallback)client_setup_request_cb, data);	
}

static gchar * janus_source_create_launch_pipe(janus_source_session * session, pipeline_callback_data_t * data) {
	gchar * launch_pipe = NULL;
	gchar * launch_pipe_video = NULL;
	gchar * launch_pipe_audio = NULL;

	g_assert(session);
	
	for (int stream = 0; stream < JANUS_SOURCE_STREAM_MAX; stream++)
	{
		int port = 0;
		janus_source_socket * sck = NULL;
		const gchar *socket_rtp_srv_name, *socket_rtcp_rcv_srv_name, *socket_rtcp_snd_srv_name;

		if (stream == JANUS_SOURCE_STREAM_VIDEO) {
			socket_rtcp_snd_srv_name = SOCKET_VIDEO_RTCP_SND_SRV;
			socket_rtp_srv_name = SOCKET_VIDEO_RTP_SRV;
		    socket_rtcp_rcv_srv_name = SOCKET_VIDEO_RTCP_RCV_SRV;
		} else {
			socket_rtcp_snd_srv_name = SOCKET_AUDIO_RTCP_SND_SRV;
			socket_rtp_srv_name = SOCKET_AUDIO_RTP_SRV;
		    socket_rtcp_rcv_srv_name = SOCKET_AUDIO_RTCP_RCV_SRV;
		}

		sck = g_hash_table_lookup(session->sockets, socket_rtcp_snd_srv_name);

		if (sck) {
			port = sck->port;
		} else {
			JANUS_LOG(LOG_ERR, "Unable to lookup for %s\n", socket_rtcp_snd_srv_name);
			return NULL;
		}

		switch (session->codec[stream])
		{
		case IDILIA_CODEC_VP8:
			launch_pipe_video = g_strdup_printf(PIPE_VIDEO_VP8,
				session->codec_pt[stream],
				socket_rtp_srv_name,
				socket_rtcp_rcv_srv_name,
				port);
			break;
		case IDILIA_CODEC_VP9:
			launch_pipe_video = g_strdup_printf(PIPE_VIDEO_VP9,
				session->codec_pt[stream],
				socket_rtp_srv_name,
				socket_rtcp_rcv_srv_name,
				port);
			break;
		case IDILIA_CODEC_H264:
			launch_pipe_video = g_strdup_printf(PIPE_VIDEO_H264,
				session->codec_pt[stream],
				socket_rtp_srv_name,
				socket_rtcp_rcv_srv_name,
				port);
			break;
		case IDILIA_CODEC_OPUS:
			launch_pipe_audio = g_strdup_printf(PIPE_AUDIO_OPUS,
				session->codec_pt[stream],
				socket_rtp_srv_name,
				socket_rtcp_rcv_srv_name,
				port);
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

static void create_server_socket(GHashTable * sockets, const gchar *name) {
	g_hash_table_insert(
			sockets, 
			(gpointer)name, 
			socket_utils_create_server_socket()
			);
}

static void create_client_socket(GHashTable * cli_sockets, const gchar *name, GHashTable * srv_sockets, const gchar *srv_name) {
	g_hash_table_insert(
		cli_sockets, 
		(gpointer)name, 
		socket_utils_create_client_socket(
			((janus_source_socket*)g_hash_table_lookup(
				srv_sockets, 
				(gpointer)srv_name))->port)
		);
}

void janus_rtsp_handle_client_callback(gpointer data) {	
	
	janus_source_session *session = (janus_source_session*)(data); 
	
	if (!session) {
		JANUS_LOG(LOG_ERR, "Session is NULL\n");
		return;
	}

	if (g_atomic_int_get(&session->hangingup) || session->destroyed) {
		JANUS_LOG(LOG_INFO, "Session is being destroyed\n");	
		return;	 
	}

	const gchar * rtsp_ip = janus_source_get_rtsp_ip();
	int rtsp_port = janus_source_rtsp_server_port(rtsp_server_data);
	pipeline_callback_data_t * callback_data = g_new0(pipeline_callback_data_t, 1);

	session->callback_data = callback_data;
	session->rtsp_url = g_strdup_printf("rtsp://%s:%d/%s", rtsp_ip, rtsp_port, session->id);

	callback_data->id = g_strdup(session->id);
	callback_data->rtsp_url = g_strdup(session->rtsp_url);

	rtsp_clients_list_init(&callback_data->clients_list, &callback_data->clients_mutex);

	session->sockets = g_hash_table_new(g_str_hash, g_str_equal);
	callback_data->sockets = g_hash_table_new(g_str_hash, g_str_equal); 

	create_server_socket(callback_data->sockets, SOCKET_VIDEO_RTP_SRV);
	create_client_socket(session->sockets, SOCKET_VIDEO_RTP_CLI, callback_data->sockets, SOCKET_VIDEO_RTP_SRV);
	create_server_socket(callback_data->sockets, SOCKET_VIDEO_RTCP_RCV_SRV);
	create_client_socket(session->sockets, SOCKET_VIDEO_RTCP_RCV_CLI, callback_data->sockets, SOCKET_VIDEO_RTCP_RCV_SRV);
	create_server_socket(session->sockets, SOCKET_VIDEO_RTCP_SND_SRV);

	create_server_socket(callback_data->sockets, SOCKET_AUDIO_RTP_SRV);
	create_client_socket(session->sockets, SOCKET_AUDIO_RTP_CLI, callback_data->sockets, SOCKET_AUDIO_RTP_SRV);
	create_server_socket(callback_data->sockets, SOCKET_AUDIO_RTCP_RCV_SRV);
	create_client_socket(session->sockets, SOCKET_AUDIO_RTCP_RCV_CLI, callback_data->sockets, SOCKET_AUDIO_RTCP_RCV_SRV);
	create_server_socket(session->sockets, SOCKET_AUDIO_RTCP_SND_SRV);

	gchar * launch_pipe = janus_source_create_launch_pipe(session, data);

	GstRTSPMediaFactory * factory = janus_source_rtsp_factory(rtsp_server_data, rtsp_ip, launch_pipe);
	g_free(launch_pipe);

	for (int stream = 0; stream < JANUS_SOURCE_STREAM_MAX; stream++)
	{
		callback_data->rtcp_cbk_data[stream].session = (gpointer)session;
		callback_data->rtcp_cbk_data[stream].is_video = (stream == JANUS_SOURCE_STREAM_VIDEO);

		janus_source_socket * sck = NULL;

		if (stream == JANUS_SOURCE_STREAM_VIDEO) {
			sck = g_hash_table_lookup(session->sockets, SOCKET_VIDEO_RTCP_SND_SRV);
		} else {
			sck = g_hash_table_lookup(session->sockets, SOCKET_AUDIO_RTCP_SND_SRV);
		}

		if (sck) {
			socket_utils_attach_callback(sck, 
				(GSourceFunc)janus_source_send_rtcp_src_received,
				(gpointer)&callback_data->rtcp_cbk_data[stream]);
			} else {
				JANUS_LOG(LOG_ERR, "Socket rtcp_snd_srv lookup error");
			}
	}

#ifdef USE_REGISTRY_SERVICE
	gchar *http_request_data = janus_source_create_json_request(session->rtsp_url);
	json_t *db_id_json_object = NULL;

	if (curl_request(session->curl_handle, session->status_service_url, http_request_data, "POST", &db_id_json_object) != TRUE) {
		JANUS_LOG(LOG_ERR, "Could not send the request to the server\n");		
	}
	else {
		if (!json_is_object(db_id_json_object)) {
			JANUS_LOG(LOG_ERR, "Not valid json object.\n");				
		}	
		else
		{			
			gint code_err = json_integer_value(json_object_get(db_id_json_object, "code"));

			if (code_err != 0) {
            	gchar *code_error_string = g_strdup_printf("%d", code_err);
				if(0 == g_strcmp0("11000", code_error_string)){
					JANUS_LOG(LOG_ERR, "The mountpoint /%s already exist in the system\n", session->id);
					janus_source_hangup_media(session->handle);		
					janus_source_send_id_error(session->handle);							
				} 
				g_free(code_error_string);
			}
			else {
				callback_data->id_media_configure_cb = g_signal_connect(factory, "media-configure", (GCallback)media_configure_cb, (gpointer)callback_data);
				callback_data->id_client_connected_cb = g_signal_connect(rtsp_server_data->rtsp_server, "client-connected", (GCallback)client_connected_cb, (gpointer)callback_data);
					
				janus_source_rtsp_add_mountpoint(rtsp_server_data, factory, session->id);
				
				session->db_entry_session_id = (gchar *) g_strdup(json_string_value(json_object_get(db_id_json_object, "_id")));
				JANUS_LOG(LOG_INFO, "Stream ready at %s\n", session->rtsp_url);
			}
		}
		json_decref(db_id_json_object);
	}
	g_free(http_request_data);

#else
	callback_data->id_media_configure_cb = g_signal_connect(factory, "media-configure", (GCallback)media_configure_cb, (gpointer)callback_data);
	callback_data->id_client_connected_cb = g_signal_connect(rtsp_server_data->rtsp_server, "client-connected", (GCallback)client_connected_cb, (gpointer)callback_data);
	janus_source_rtsp_add_mountpoint(rtsp_server_data, factory, session->id);	
	session->db_entry_session_id = NULL;
	JANUS_LOG(LOG_INFO, "Stream ready at %s\n", session->rtsp_url);
#endif
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
