#pragma once
#include <glib.h>
#include <gst/rtsp-server/rtsp-server.h>

#include "pipeline_callback_data.h"

/* Used by rtsp server thread */
typedef struct janus_source_rtsp_server_data
{
	GstRTSPServer *rtsp_server;
	GAsyncQueue *rtsp_async_queue;
	GSource *rtsp_queue_source ;
	GMainLoop *loop;
} janus_source_rtsp_server_data;

void janus_source_attach_rtsp_queue_callback(janus_source_rtsp_server_data *rtsp_server,  GSourceFunc callback, GMainContext *context);
void janus_source_deattach_rtsp_queue_callback(janus_source_rtsp_server_data *rtsp_server);
void janus_source_rtsp_create_and_run_main_loop(janus_source_rtsp_server_data *rtsp_server, GMainContext * context);
void janus_source_rtsp_clean_and_quit_main_loop(janus_source_rtsp_server_data *rtsp_server);

void janus_source_create_rtsp_server_and_queue(janus_source_rtsp_server_data *rtsp_server, GMainContext *context);
GstRTSPMediaFactory * janus_source_rtsp_factory(janus_source_rtsp_server_data *rtsp_server, const gchar * local_ip, gchar * launch_pipe);
void janus_source_rtsp_add_mountpoint(janus_source_rtsp_server_data *rtsp_server , GstRTSPMediaFactory *factory, gchar * id);
void janus_source_rtsp_remove_mountpoint(janus_source_rtsp_server_data *rtsp_server, gchar * id, pipeline_callback_data_t *data);
int janus_source_rtsp_server_port(janus_source_rtsp_server_data *rtsp_server);

void janus_source_close_all_rtsp_sessions(janus_source_rtsp_server_data *rtsp_server);
