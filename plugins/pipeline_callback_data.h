#pragma once

#include <gst/gst.h>

enum
{
	JANUS_SOURCE_STREAM_VIDEO = 0,
	JANUS_SOURCE_STREAM_AUDIO,
	JANUS_SOURCE_STREAM_MAX
};


typedef struct rtcp_callback_data
{
	gpointer * session;
	gboolean is_video;
} janus_source_rtcp_cbk_data;

typedef struct {
    gchar * id;
    gchar *rtsp_url;
	janus_source_rtcp_cbk_data rtcp_cbk_data[JANUS_SOURCE_STREAM_MAX];
    GHashTable * sockets;
	gulong id_media_configure_cb;
	gulong id_client_connected_cb;
    gulong id_rtsp_media_target_state_cb;
	GList * clients_list;
	GMutex clients_mutex;
} pipeline_callback_data_t;

