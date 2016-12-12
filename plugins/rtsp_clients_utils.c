#include "debug.h"
#include "rtsp_clients_utils.h"

static void rtsp_server_send_teardown(GstRTSPClient *client, const gchar * url);

static void rtsp_server_send_teardown_for_client_session(GstRTSPClient *client, GstRTSPSession *session, const gchar * url)
{
	GstRTSPResult res;
	GstRTSPMessage *msg;

	JANUS_LOG(LOG_INFO, "Sending TEARDOWN url=%s; session=%p\n", url, session);

	gst_rtsp_message_new (&msg);
	gst_rtsp_message_init_request (msg, GST_RTSP_TEARDOWN, url);
	res = gst_rtsp_client_send_message (client, session, msg); 

	if (res != GST_RTSP_OK) {
		JANUS_LOG(LOG_ERR, "Sending TEARDOWN failed for url=%s; session=%p\n", url, session);
	}
}

static GstRTSPFilterResult rtsp_server_send_teardown_func (GstRTSPClient *client, GstRTSPSession *sess, gpointer user_data) 
{
	const gchar * url = (const gchar *)user_data;
	rtsp_server_send_teardown_for_client_session(client, sess, url);
	return GST_RTSP_FILTER_REMOVE;
}

static void rtsp_server_send_teardown(GstRTSPClient *client, const gchar * url) 
{
	GList * list = gst_rtsp_client_session_filter (client,
									rtsp_server_send_teardown_func,
									(gpointer)url);
	g_list_free_full(list, gst_object_unref);
}

void rtsp_clients_list_init(GList **list, GMutex *mutex)
{
	*list = NULL;
	g_mutex_init (mutex);
}

void rtsp_clients_list_add(GList **list, GMutex *mutex, GstRTSPClient *client)
{
	if (mutex && list) {
		JANUS_LOG(LOG_VERB, "Adding RTSP client to clients list\n");
		g_mutex_lock (mutex);
		*list = g_list_append(*list, g_object_ref(client));
		g_mutex_unlock (mutex);
	}
}

void rtsp_clients_list_remove(GList **list, GMutex *mutex, GstRTSPClient *client)
{

	if (mutex && *list) {
		JANUS_LOG(LOG_VERB, "Removing RTSP client from clients list\n");
		g_mutex_lock (mutex);
		*list = g_list_remove(*list, client);
		g_mutex_unlock (mutex);
		g_object_unref(client);
	}
}

void rtsp_clients_teardown_and_remove_all(GList **list, GMutex *mutex, gchar *uri)
{
	JANUS_LOG(LOG_VERB, "Sending TEARDOWN and closing RTSP clients: %s\n", uri);

	g_mutex_lock (mutex);
	GList *l = *list;

	while (l != NULL)
	{
		GList *next = l->next;
		GstRTSPClient *client = l->data;

		if (client != NULL) {
			rtsp_server_send_teardown(client, uri);
			gst_rtsp_client_close(client);
			g_object_unref(client);
			l->data = NULL;
		}

		*list = g_list_delete_link (*list, l);
		l = next;
	}
	g_mutex_unlock (mutex);
}

void rtsp_clients_list_destroy(GList **list, GMutex *mutex)
{
	JANUS_LOG(LOG_VERB, "Destroying RTSP clients list\n");
	if (*list) {
		g_list_free_full (*list, g_object_unref);
		*list = NULL;
	}
}
