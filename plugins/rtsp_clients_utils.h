#pragma once

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

void rtsp_clients_list_init(GList **list, GMutex *mutex);
void rtsp_clients_list_add(GList **list, GMutex *mutex, GstRTSPClient *client);
void rtsp_clients_list_remove(GList **list, GMutex *mutex, GstRTSPClient *client);
void rtsp_clients_teardown_and_remove_all(GList **list, GMutex *mutex, gchar *uri);
void rtsp_clients_list_destroy(GList **list, GMutex *mutex);
