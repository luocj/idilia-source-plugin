#include "rtsp_server.h"
#include "queue_callbacks.h"

void create_rtsp_server_and_queue(janus_source_rtsp_server_data *rtsp_server){
	rtsp_server->rtsp_server = gst_rtsp_server_new();
	rtsp_server->rtsp_async_queue =  g_async_queue_new();
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


