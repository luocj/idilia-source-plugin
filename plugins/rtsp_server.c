#include "rtsp_server.h"
#include "queue_callbacks.h"
#include "gst_utils.h"
#include "debug.h"


static const char *RTSP_PORT_NUMBER = "3554"; 
static GstRTSPFilterResult janus_source_close_rtsp_sessions(GstRTSPSessionPool *pool, GstRTSPSession *session, gpointer user_data);
static void janus_source_close_all_rtsp_sessions_for_mountpoint(GstRTSPServer *rtsp_server, gchar *uri);

void janus_source_create_rtsp_server_and_queue(janus_source_rtsp_server_data *rtsp_server, GMainContext *context){
	rtsp_server->rtsp_server = gst_rtsp_server_new();
	
	/* Allocate random port */
	gst_rtsp_server_set_service(rtsp_server->rtsp_server, RTSP_PORT_NUMBER);

	/* attach the server to the thread-default context */
	if (gst_rtsp_server_attach(rtsp_server->rtsp_server, context) == 0) {
		JANUS_LOG(LOG_ERR, "Failed to attach the server\n");
	}

	rtsp_server->rtsp_async_queue =  g_async_queue_new();
}

GstRTSPMediaFactory * janus_source_rtsp_factory(janus_source_rtsp_server_data *rtsp_server, const gchar * local_ip, gchar * launch_pipe) {
	GstRTSPMediaFactory * factory;
	gst_rtsp_server_set_address(rtsp_server->rtsp_server, local_ip);
	factory = gst_rtsp_media_factory_new();
	gst_rtsp_media_factory_set_latency(factory, 0);
	gst_rtsp_media_factory_set_profiles(factory, GST_RTSP_PROFILE_AVPF);
	/* store up to 100ms of retransmission data */
	gst_rtsp_media_factory_set_retransmission_time(factory, 100 * GST_MSECOND);	
	gst_rtsp_media_factory_set_launch(factory, launch_pipe);	
	/* media created from this factory can be shared between clients */
	gst_rtsp_media_factory_set_shared(factory, TRUE);
	return factory;
}

void janus_source_rtsp_add_mountpoint(janus_source_rtsp_server_data *rtsp_server , GstRTSPMediaFactory *factory, gchar * id){ 	
	JANUS_LOG(LOG_INFO, "Adding mountpoint: /%s\n", id);
	gchar * uri = g_strdup_printf("/%s", id);
	GstRTSPMountPoints *mounts;
	/* get the default mount points from the server */
	mounts = gst_rtsp_server_get_mount_points(rtsp_server->rtsp_server);	
	/* attach the session to the "/camera" URL */	
	gst_rtsp_mount_points_add_factory(mounts, uri, factory);
	g_object_unref(mounts);	
	g_free(uri);
}

void janus_source_rtsp_remove_mountpoint(janus_source_rtsp_server_data *rtsp_server, gchar * id, pipeline_callback_data_t *data){ 	
	JANUS_LOG(LOG_INFO, "Remove mountpoint: /%s\n", id);

	gchar * uri = g_strdup_printf("/%s", id);

	rtsp_clients_teardown_and_remove_all(&data->clients_list, &data->clients_mutex, data->rtsp_url);

	if (data->id_client_connected_cb > 0) {
		JANUS_LOG(LOG_VERB, "Disconnecting id_client_connected_cb signal %lu\n", data->id_client_connected_cb);
		g_signal_handler_disconnect (rtsp_server->rtsp_server, data->id_client_connected_cb);
		data->id_client_connected_cb = 0;
	}

	g_assert(GST_IS_RTSP_SERVER(rtsp_server->rtsp_server));
	janus_source_close_all_rtsp_sessions_for_mountpoint(rtsp_server->rtsp_server, uri);

	/* get the default mount points from the server */
	GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(rtsp_server->rtsp_server);	

	if (data->id_media_configure_cb > 0) {

		GstRTSPMediaFactory * factory = gst_rtsp_mount_points_match(mounts, uri, NULL);

		if (factory) {
			JANUS_LOG(LOG_VERB, "Disconnecting id_media_configure_cb signal %lu\n", data->id_media_configure_cb);
			g_signal_handler_disconnect (factory, data->id_media_configure_cb);
			data->id_media_configure_cb = 0;
			g_object_unref(factory);
		}
	}

	/* remove the factory for the uri */	
	g_print("Remove mount: %s\n", uri);
	gst_rtsp_mount_points_remove_factory(mounts, uri);
	g_object_unref(mounts);	

	pipeline_callback_data_destroy(data);
	g_free(uri);
}

int janus_source_rtsp_server_port(janus_source_rtsp_server_data *rtsp_server){
	return gst_rtsp_server_get_bound_port(rtsp_server->rtsp_server); 
}


void janus_source_attach_rtsp_queue_callback(janus_source_rtsp_server_data *rtsp_server,  GSourceFunc callback, GMainContext *context) {
	rtsp_server->rtsp_queue_source = queue_source_new(rtsp_server->rtsp_async_queue);
	g_assert(rtsp_server->rtsp_queue_source);
	g_source_attach(rtsp_server->rtsp_queue_source, context) ;
	g_source_set_callback(rtsp_server->rtsp_queue_source, callback, NULL, NULL) ;
}

void janus_source_deattach_rtsp_queue_callback(janus_source_rtsp_server_data *rtsp_server) {
	g_source_destroy(rtsp_server->rtsp_queue_source);
	g_source_unref(rtsp_server->rtsp_queue_source);
	g_async_queue_unref (rtsp_server->rtsp_async_queue);
	rtsp_server->rtsp_queue_source = NULL;
}

void janus_source_rtsp_create_and_run_main_loop(janus_source_rtsp_server_data *rtsp_server, GMainContext * context) {
	GMainLoop *loop;
	loop = g_main_loop_new(context, FALSE);
	rtsp_server->loop = loop;
	g_main_loop_run(loop);
}

void janus_source_rtsp_clean_and_quit_main_loop(janus_source_rtsp_server_data *rtsp_server) {
	JANUS_LOG(LOG_VERB, "janus_source_rtsp_clean_and_quit_main_loop\n");
	if (g_main_loop_is_running (rtsp_server->loop)) {	
		g_main_loop_quit(rtsp_server->loop);
		g_main_loop_unref(rtsp_server->loop);
		rtsp_server->loop = NULL; 
	}
}

void janus_source_close_all_rtsp_sessions(janus_source_rtsp_server_data *rtsp_server) {
	JANUS_LOG(LOG_VERB, "janus_source_close_all_rtsp_sessions\n");
	GList * sessions_list;
	GstRTSPSessionPool *session_pool;
	session_pool = gst_rtsp_server_get_session_pool(rtsp_server->rtsp_server);
	sessions_list = gst_rtsp_session_pool_filter(session_pool, janus_source_close_rtsp_sessions, NULL);
	g_list_free_full(sessions_list, gst_object_unref);
	g_object_unref(session_pool);
}

static GstRTSPFilterResult
janus_source_close_rtsp_sessions(GstRTSPSessionPool *pool, GstRTSPSession *session, gpointer user_data) { 
	GstRTSPFilterResult result = GST_RTSP_FILTER_KEEP;
	gchar * uri = (gchar*)user_data;

	if (uri == NULL) {
		result = GST_RTSP_FILTER_REMOVE;	
	} else {
		gint matched = 0;
		gst_rtsp_session_get_media(session, uri, &matched);

		if (strlen(uri) == (guint)matched) {
			result = GST_RTSP_FILTER_REMOVE;
		}
	}

	if (result == GST_RTSP_FILTER_REMOVE) {
		JANUS_LOG(LOG_VERB, "Removing RTSP session: %s\n", gst_rtsp_session_get_sessionid(session));
	}
	return result;
}

static void janus_source_close_all_rtsp_sessions_for_mountpoint(GstRTSPServer *rtsp_server, gchar *uri) {
	JANUS_LOG(LOG_VERB, "janus_source_close_all_rtsp_sessions_for_mountpoint: %s\n", uri);
	GList * sessions_list;
	GstRTSPSessionPool *session_pool;
	session_pool = gst_rtsp_server_get_session_pool(rtsp_server);
	sessions_list = gst_rtsp_session_pool_filter(session_pool, janus_source_close_rtsp_sessions, uri);
	g_list_free_full(sessions_list, gst_object_unref);
	g_object_unref(session_pool);
}

