#pragma once

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include "idilia_source_common.h"

void media_configure_cb(GstRTSPMediaFactory * factory, GstRTSPMedia * media, gpointer data);
void client_connected_cb(GstRTSPServer *gstrtspserver, GstRTSPClient *gstrtspclient, gpointer data);
gchar * janus_source_create_launch_pipe(janus_source_session * session);
GstRTSPFilterResult janus_source_close_rtsp_sessions(GstRTSPSessionPool *pool, GstRTSPSession *session, gpointer data);
gboolean request_key_frame_periodic_cb(gpointer data);
void janus_rtsp_handle_client_callback(gpointer data);