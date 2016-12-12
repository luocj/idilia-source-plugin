#pragma once

#include <gst/gst.h>
#include "socket_utils.h"
#include "idilia_source_common.h"

gboolean request_key_frame_periodic_cb(gpointer data);
void janus_rtsp_handle_client_callback(gpointer data);
void pipeline_callback_data_destroy(pipeline_callback_data_t * data);
int close_and_destroy_sockets(gpointer key, janus_source_socket * sck, gpointer user_data);

