/*! \file   idilia_source.c
* \author Lorenzo Miniero <lorenzo@meetecho.com>
*         Tomasz Zajac <tomasz.zajac@motorolasolutions.com>
* \copyright GNU General Public License v3m
* \brief  Idilia source plugin
* \details  This is a trivial SourcePlugin for Janus, just used to
* showcase the plugin interface. A peer attaching to this plugin will
* receive back the same RTP packets and RTCP messages he sends: the
* RTCP messages, of course, would be modified on the way by the gateway
* to make sure they are coherent with the involved SSRCs. In order to
* demonstrate how peer-provided messages can change the behaviour of a
* plugin, this plugin implements a simple API based on three messages:
*
* 1. a message to enable/disable audio (that is, to tell the plugin
* whether incoming audio RTP packets need to be sent back or discarded);
* 2. a message to enable/disable video (that is, to tell the plugin
* whether incoming video RTP packets need to be sent back or discarded);
* 3. a message to cap the bitrate (which would modify incoming RTCP
* REMB messages before sending them back, in order to trick the peer into
* thinking the available bandwidth is different).
*
* \section sourceapi Source Plugin API
*
* There's a single unnamed request you can send and it's asynchronous,
* which means all responses (successes and errors) will be delivered
* as events with the same transaction.
*
* The request has to be formatted as follows. All the attributes are
* optional, so any request can contain a subset of them:
*
\verbatim
{
"audio" : true|false,
"video" : true|false,
"bitrate" : <numeric bitrate value>,
"record" : true|false,
"filename" : <base path/filename to use for the recording>
}
\endverbatim
*
* \c audio instructs the plugin to do or do not bounce back audio
* frames; \c video does the same for video; \c bitrate caps the
* bandwidth to force on the browser encoding side (e.g., 128000 for
* 128kbps).
*
* The first request must be sent together with a JSEP offer to
* negotiate a PeerConnection: a JSEP answer will be provided with
* the asynchronous response notification. Subsequent requests (e.g., to
* dynamically manipulate the bitrate while testing) have to be sent
* without any JSEP payload attached.
*
* A successful request will result in an \c ok event:
*
\verbatim
{
"source" : "event",
"result": "ok"
}
\endverbatim
*
* An error instead will provide both an error code and a more verbose
* description of the cause of the issue:
*
\verbatim
{
"source" : "event",
"error_code" : <numeric ID, check Macros below>,
"error" : "<error description as a string>"
}
\endverbatim
*
* If the plugin detects a loss of the associated PeerConnection, a
* "done" notification is triggered to inform the application the Source
* Plugin session is over:
*
\verbatim
{
"source" : "event",JANUS
"result": "done"
}
\endverbatim
*
* \ingroup plugins
* \ref plugins
*/

#include "plugin.h"

#include <jansson.h>
#include "../debug.h"
#include "../apierror.h"
#include "../config.h"
#include "../mutex.h"
#include "../record.h"
#include "../rtcp.h"
#include "../utils.h"
#include <sys/socket.h>

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include "node_service_access.h"
#include "sdp_utils.h"
#include "socket_utils.h"
#include "queue_callbacks.h"
#include "audio_video_defines.h"
#include "rtsp_server.h"
/* Plugin information */
#define JANUS_SOURCE_VERSION			1
#define JANUS_SOURCE_VERSION_STRING	    "0.0.1"
#define JANUS_SOURCE_DESCRIPTION		"Idilia source plugin"
#define JANUS_SOURCE_NAME		        "Idilia Source plugin"
#define JANUS_SOURCE_AUTHOR			    "Motorola Solutions Inc."
#define JANUS_SOURCE_PACKAGE			"idilia.plugin.source"

//#define PLI_WORKAROUND

/* Plugin methods */
janus_plugin *create(void);
int janus_source_init(janus_callbacks *callback, const char *config_path);
void janus_source_destroy(void);
int janus_source_get_api_compatibility(void);
int janus_source_get_version(void);
const char *janus_source_get_version_string(void);
const char *janus_source_get_description(void);
const char *janus_source_get_name(void);
const char *janus_source_get_author(void);
const char *janus_source_get_package(void);
void janus_source_create_session(janus_plugin_session *handle, int *error);
struct janus_plugin_result *janus_source_handle_message(janus_plugin_session *handle, char *transaction, char *message, char *sdp_type, char *sdp);
void janus_source_setup_media(janus_plugin_session *handle);
void janus_source_incoming_rtp(janus_plugin_session *handle, int video, char *buf, int len);
void janus_source_incoming_rtcp(janus_plugin_session *handle, int video, char *buf, int len);
void janus_source_incoming_data(janus_plugin_session *handle, char *buf, int len);
void janus_source_slow_link(janus_plugin_session *handle, int uplink, int video);
void janus_source_hangup_media(janus_plugin_session *handle);
void janus_source_destroy_session(janus_plugin_session *handle, int *error);
char *janus_source_query_session(janus_plugin_session *handle);
extern gboolean queue_events_callback(gpointer data);
/* Plugin setup */
static janus_plugin janus_source_plugin =
JANUS_PLUGIN_INIT(
	.init = janus_source_init,
	.destroy = janus_source_destroy,

	.get_api_compatibility = janus_source_get_api_compatibility,
	.get_version = janus_source_get_version,
	.get_version_string = janus_source_get_version_string,
	.get_description = janus_source_get_description,
	.get_name = janus_source_get_name,
	.get_author = janus_source_get_author,
	.get_package = janus_source_get_package,

	.create_session = janus_source_create_session,
	.handle_message = janus_source_handle_message,
	.setup_media = janus_source_setup_media,
	.incoming_rtp = janus_source_incoming_rtp,
	.incoming_rtcp = janus_source_incoming_rtcp,
	.incoming_data = janus_source_incoming_data,
	.slow_link = janus_source_slow_link,
	.hangup_media = janus_source_hangup_media,
	.destroy_session = janus_source_destroy_session,
	.query_session = janus_source_query_session,
	);

/* Plugin creator */
janus_plugin *create(void) {
	JANUS_LOG(LOG_VERB, "%s created!\n", JANUS_SOURCE_NAME);
	return &janus_source_plugin;
}


/* Useful stuff */
static volatile gint initialized = 0, stopping = 0;
static janus_callbacks *gateway = NULL;
static GThread *handler_thread;
static GThread *watchdog;
static GThread *handler_rtsp_thread;
static void *janus_source_handler(void *data);



typedef struct janus_source_message {
	janus_plugin_session *handle;
	char *transaction;
	char *message;
	char *sdp_type;
	char *sdp;
} janus_source_message;
static GAsyncQueue *messages = NULL;
static janus_source_message exit_message;

typedef struct rtcp_callback_data
{
	gpointer * session;
	gboolean is_video;
} janus_source_rtcp_cbk_data;

enum
{
	JANUS_SOURCE_STREAM_VIDEO = 0,
	JANUS_SOURCE_STREAM_AUDIO,
	JANUS_SOURCE_STREAM_MAX
};

enum
{
	JANUS_SOURCE_SOCKET_RTP_SRV = 0,
	JANUS_SOURCE_SOCKET_RTP_CLI,
	JANUS_SOURCE_SOCKET_RTCP_RCV_SRV,
	JANUS_SOURCE_SOCKET_RTCP_RCV_CLI,
	JANUS_SOURCE_SOCKET_RTCP_SND_SRV,
	JANUS_SOURCE_SOCKET_MAX
};

typedef struct janus_source_session {
	janus_plugin_session *handle;
	gboolean audio_active;
	gboolean video_active;
	uint64_t bitrate;
	janus_recorder *arc;	/* The Janus recorder instance for this user's audio, if enabled */
	janus_recorder *vrc;	/* The Janus recorder instance for this user's video, if enabled */
	janus_mutex rec_mutex;	/* Mutex to protect the recorders from race conditions */
	guint16 slowlink_count;
	volatile gint hangingup;
	gint64 destroyed;	/* Time at which this session was marked as destroyed */
	gchar * db_entry_session_id;
#ifdef PLI_WORKAROUND
	gint periodic_pli;
	GstState rtsp_state;
#endif
	janus_source_socket socket[JANUS_SOURCE_STREAM_MAX][JANUS_SOURCE_SOCKET_MAX];
	janus_source_rtcp_cbk_data rtcp_cbk_data[JANUS_SOURCE_STREAM_MAX];
	idilia_codec codec[JANUS_SOURCE_STREAM_MAX];
	gint codec_pt[JANUS_SOURCE_STREAM_MAX];
} janus_source_session;

static GHashTable *sessions;
static GList *old_sessions;
static janus_mutex sessions_mutex;
static GHashTable *sessions;
static CURL *curl_handle = NULL;
static const char * gst_debug_str = "*:3"; //gst debug setting

/* configuration options */
static uint16_t udp_min_port = 0, udp_max_port = 0;
static gchar *status_service_url = NULL;
static gint pli_period = 0;
static gboolean use_codec_priority = FALSE;
static idilia_codec codec_priority_list[] = { IDILIA_CODEC_INVALID, IDILIA_CODEC_INVALID };
//static GstRTSPServer *rtsp_server;
//GAsyncQueue *rtsp_async_queue;
static janus_source_rtsp_server_data *rtsp_server_data = NULL;
//function declarations
static void *janus_source_rtsp_server_thread(void *data);
static void client_connected_cb(GstRTSPServer *gstrtspserver, GstRTSPClient *gstrtspclient, gpointer data);
#ifdef PLI_WORKAROUND
static void janus_source_request_keyframe(janus_source_session *session);
static gboolean request_key_frame_cb(gpointer data);
static gboolean request_key_frame_periodic_cb(gpointer data);
static gboolean request_key_frame_if_not_playing_cb(gpointer data);
#endif
static void media_configure_cb(GstRTSPMediaFactory * factory, GstRTSPMedia * media, gpointer data);
static void client_play_request_cb(GstRTSPClient  *gstrtspclient, GstRTSPContext *rtspcontext, gpointer data);
static void janus_source_close_session_func(gpointer key, gpointer value, gpointer user_data);
static void janus_source_close_session(janus_source_session * session);
static void janus_source_relay_rtp(janus_source_session *session, int video, char *buf, int len);
static void janus_source_relay_rtcp(janus_source_session *session, int video, char *buf, int len);
static void janus_source_parse_ports_range(janus_config_item *ports_range, uint16_t * udp_min_port, uint16_t * udp_max_port);
static void janus_source_parse_video_codec_priority(janus_config_item *config);
static GstRTSPFilterResult janus_source_close_rtsp_sessions(GstRTSPSessionPool *pool, GstRTSPSession *session, gpointer data);
static void janus_source_parse_status_service_url(janus_config_item *config_url, gchar **url);
static void janus_source_parse_pli_period(janus_config_item *config_pli, gint *pli_period);
static gboolean janus_source_send_rtcp_src_received(GSocket *socket, GIOCondition condition, janus_source_rtcp_cbk_data * data);
static gchar * janus_source_do_codec_negotiation(janus_source_session * session, gchar * orig_sdp);
static idilia_codec janus_source_select_video_codec_by_priority_list(const gchar * sdp);
static GstSDPMessage * create_sdp(GstRTSPClient * client, GstRTSPMedia * media);
static void rtsp_callback(gpointer data);
static const gchar * janus_source_get_udpsrc_name(int stream, int type);
static gboolean janus_source_create_sockets(janus_source_socket socket[JANUS_SOURCE_STREAM_MAX][JANUS_SOURCE_SOCKET_MAX]);


/* External declarations (janus.h) */
extern gchar *janus_get_local_ip(void);
extern gchar *janus_get_public_ip(void);

static void janus_source_message_free(janus_source_message *msg) {
	if (!msg || msg == &exit_message)
		return;

	msg->handle = NULL;

	g_free(msg->transaction);
	msg->transaction = NULL;
	g_free(msg->message);
	msg->message = NULL;
	g_free(msg->sdp_type);
	msg->sdp_type = NULL;
	g_free(msg->sdp);
	msg->sdp = NULL;

	g_free(msg);
}


/* Error codes */
#define JANUS_SOURCE_ERROR_NO_MESSAGE		411
#define JANUS_SOURCE_ERROR_INVALID_JSON		412
#define JANUS_SOURCE_ERROR_INVALID_ELEMENT	413


/* SourcePlugin watchdog/garbage collector (sort of) */
void *janus_source_watchdog(void *data);
void *janus_source_watchdog(void *data) {
	JANUS_LOG(LOG_INFO, "SourcePlugin watchdog started\n");
	gint64 now = 0;
	while (g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
		janus_mutex_lock(&sessions_mutex);
		/* Iterate on all the sessions */
		now = janus_get_monotonic_time();
		if (old_sessions != NULL) {
			GList *sl = old_sessions;
			JANUS_LOG(LOG_HUGE, "Checking %d old SourcePlugin sessions...\n", g_list_length(old_sessions));
			while (sl) {
				janus_source_session *session = (janus_source_session *)sl->data;
				if (!session) {
					sl = sl->next;
					continue;
				}
				if (now - session->destroyed >= 5 * G_USEC_PER_SEC) {
					/* We're lazy and actually get rid of the stuff only after a few seconds */
					JANUS_LOG(LOG_VERB, "Freeing old SourcePlugin session\n");
					GList *rm = sl->next;
					old_sessions = g_list_delete_link(old_sessions, sl);
					sl = rm;
					session->handle = NULL;
					g_free(session);
					session = NULL;
					continue;
				}
				sl = sl->next;
			}
		}
		janus_mutex_unlock(&sessions_mutex);
		g_usleep(500000);
	}
	JANUS_LOG(LOG_INFO, "SourcePlugin watchdog stopped\n");
	return NULL;
}

/* Plugin implementation */
int janus_source_init(janus_callbacks *callback, const char *config_path) {
	if (g_atomic_int_get(&stopping)) {
		/* Still stopping from before */
		return -1;
	}
	if (callback == NULL || config_path == NULL) {
		/* Invalid arguments */
		return -1;
	}

	/* Read configuration */
	char filename[255];
	g_snprintf(filename, 255, "%s/%s.cfg", config_path, JANUS_SOURCE_PACKAGE);
	JANUS_LOG(LOG_VERB, "Configuration file: %s\n", filename);
	janus_config *config = janus_config_parse(filename);
	if (config != NULL)
		janus_config_print(config);

	/* Parse configuration */
	if (config != NULL)
	{
		GList *cl = janus_config_get_categories(config);
		while (cl != NULL)
		{
			janus_config_category *cat = (janus_config_category *)cl->data;
			if (cat->name == NULL)
			{
				cl = cl->next;
				continue;
			}
			JANUS_LOG(LOG_VERB, "Parsing category '%s'\n", cat->name);
			janus_source_parse_ports_range(janus_config_get_item(cat, "udp_port_range"), &udp_min_port, &udp_max_port);
			janus_source_parse_status_service_url(janus_config_get_item(cat,"status_service_url"),&status_service_url);
			janus_source_parse_pli_period(janus_config_get_item(cat, "pli_period"), &pli_period);
			janus_source_parse_video_codec_priority(janus_config_get_item(cat, "video_codec_priority"));
			
			cl = cl->next;
		}
		janus_config_destroy(config);
		config = NULL;
	}

	if (udp_min_port <= 0 || udp_max_port <= 0) {
		udp_min_port = 4000;
		udp_max_port = 5000;
		JANUS_LOG(LOG_WARN, "Using default port range: %d-%d\n", udp_min_port, udp_max_port);
	}

	sessions = g_hash_table_new(NULL, NULL);
	janus_mutex_init(&sessions_mutex);
	messages = g_async_queue_new_full((GDestroyNotify)janus_source_message_free);
	/* This is the callback we'll need to invoke to contact the gateway */
	gateway = callback;
	g_atomic_int_set(&initialized, 1);

	GError *error = NULL;
	/* Start the sessions watchdog */
	watchdog = g_thread_try_new("source watchdog", &janus_source_watchdog, NULL, &error);
	if (error != NULL) {
		g_atomic_int_set(&initialized, 0);
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the SourcePlugin watchdog thread...\n", error->code, error->message ? error->message : "??");
		return -1;
	}

	gst_init(NULL, NULL);
	gst_debug_set_threshold_from_string(gst_debug_str, FALSE);

	curl_handle = curl_init();
	
	socket_utils_init(udp_min_port, udp_max_port);
	
	/* Launch the thread that will handle incoming messages */
	handler_thread = g_thread_try_new("janus source handler", janus_source_handler, NULL, &error);
	if (error != NULL) {
		g_atomic_int_set(&initialized, 0);
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the Source handler thread...\n", error->code, error->message ? error->message : "??");
		return -1;
	}
	
	/* Launch the thread that will handle rtsp clients */
	handler_rtsp_thread = g_thread_try_new("rtsp server", janus_source_rtsp_server_thread, NULL, &error); 
	if (error != NULL) {
		g_atomic_int_set(&initialized, 0);
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the Source rtsp server thread...\n", error->code, error->message ? error->message : "??");
		return -1;
	}

	JANUS_LOG(LOG_INFO, "%s initialized!\n", JANUS_SOURCE_NAME);
	return 0;
}


void janus_source_destroy(void) {
	if (!g_atomic_int_get(&initialized))
		return;
	g_atomic_int_set(&stopping, 1);

	g_async_queue_push(messages, &exit_message);
	if (handler_thread != NULL) {
		g_thread_join(handler_thread);
		handler_thread = NULL;
	}


	janus_source_deattach_rtsp_queue_callback(rtsp_server_data);
	
	if (g_main_loop_is_running (rtsp_server_data->loop)) {	
		g_main_loop_quit(rtsp_server_data->loop);
		g_main_loop_unref(rtsp_server_data->loop); 
	}

	if (handler_rtsp_thread != NULL) {
		g_thread_join(handler_rtsp_thread);
		handler_rtsp_thread = NULL;
	}

	if (watchdog != NULL) {
		g_thread_join(watchdog);
		watchdog = NULL;
	}

	g_hash_table_foreach(sessions, janus_source_close_session_func, NULL);
	socket_utils_destroy();
	
	/* FIXME We should destroy the sessions cleanly */
	janus_mutex_lock(&sessions_mutex);
	g_hash_table_destroy(sessions);
	janus_mutex_unlock(&sessions_mutex);
	g_async_queue_unref(messages);
	messages = NULL;
	sessions = NULL;
	
    /* Free configuration fields */
    if (status_service_url) {
        g_free(status_service_url);
		status_service_url = NULL;
    }
	curl_cleanup(curl_handle);
	
	g_atomic_int_set(&initialized, 0);
	g_atomic_int_set(&stopping, 0);
	JANUS_LOG(LOG_INFO, "%s destroyed!\n", JANUS_SOURCE_NAME);
}

int janus_source_get_api_compatibility(void) {
	/* Important! This is what your plugin MUST always return: don't lie here or bad things will happen */
	return JANUS_PLUGIN_API_VERSION;
}

int janus_source_get_version(void) {
	return JANUS_SOURCE_VERSION;
}

const char *janus_source_get_version_string(void) {
	return JANUS_SOURCE_VERSION_STRING;
}

const char *janus_source_get_description(void) {
	return JANUS_SOURCE_DESCRIPTION;
}

const char *janus_source_get_name(void) {
	return JANUS_SOURCE_NAME;
}

const char *janus_source_get_author(void) {
	return JANUS_SOURCE_AUTHOR;
}

const char *janus_source_get_package(void) {
	return JANUS_SOURCE_PACKAGE;
}

void janus_source_create_session(janus_plugin_session *handle, int *error) {

	if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}
	janus_source_session *session = (janus_source_session *)g_malloc0(sizeof(janus_source_session));
	if (session == NULL) {
		JANUS_LOG(LOG_FATAL, "Memory error!\n");
		*error = -2;
		return;
	}
	session->handle = handle;
	session->audio_active = TRUE;
	session->video_active = TRUE;

	for (int stream = 0; stream < JANUS_SOURCE_STREAM_MAX; stream++)
	{
		session->codec[stream] = IDILIA_CODEC_INVALID;
		session->codec_pt[stream] = -1;
	}

#ifdef PLI_WORKAROUND
	session->periodic_pli = 0;
	session->rtsp_state = GST_STATE_NULL;
#endif

	janus_mutex_init(&session->rec_mutex);
	session->bitrate = 0;	/* No limit */
	session->destroyed = 0;
	g_atomic_int_set(&session->hangingup, 0);
	handle->plugin_handle = session;

	memset(session->socket, 0, sizeof(session->socket));
	if (!janus_source_create_sockets(session->socket))
	{
		JANUS_LOG(LOG_FATAL, "Unable to create one or more sockets!\n");
		*error = -3;
	}

	janus_mutex_lock(&sessions_mutex);
	g_hash_table_insert(sessions, handle, session);
	janus_mutex_unlock(&sessions_mutex);

	return;
}

void janus_source_destroy_session(janus_plugin_session *handle, int *error) {
	if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}


	janus_source_session *session = (janus_source_session *)handle->plugin_handle;
	if (!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		*error = -2;
		return;
	}
	JANUS_LOG(LOG_VERB, "Removing Source Plugin session...\n");
	janus_source_close_session(session);

	janus_mutex_lock(&sessions_mutex);
	if (!session->destroyed) {
		session->destroyed = janus_get_monotonic_time();
		g_hash_table_remove(sessions, handle);
		/* Cleaning up and removing the session is done in a lazy way */
		old_sessions = g_list_append(old_sessions, session);
	}
	janus_mutex_unlock(&sessions_mutex);
	return;
}

char *janus_source_query_session(janus_plugin_session *handle) {
	if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		return NULL;
	}
	janus_source_session *session = (janus_source_session *)handle->plugin_handle;
	if (!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return NULL;
	}
	/* In the source plugin, every session is the same: we just provide some configure info */
	json_t *info = json_object();
	json_object_set_new(info, "audio_active", json_string(session->audio_active ? "true" : "false"));
	json_object_set_new(info, "video_active", json_string(session->video_active ? "true" : "false"));
	json_object_set_new(info, "bitrate", json_integer(session->bitrate));
#if 0 //recording
	if (session->arc || session->vrc) {
		json_t *recording = json_object();
		if (session->arc && session->arc->filename)
			json_object_set_new(recording, "audio", json_string(session->arc->filename));
		if (session->vrc && session->vrc->filename)
			json_object_set_new(recording, "video", json_string(session->vrc->filename));
		json_object_set_new(info, "recording", recording);
	}
#endif
	json_object_set_new(info, "slowlink_count", json_integer(session->slowlink_count));
	json_object_set_new(info, "destroyed", json_integer(session->destroyed));
	char *info_text = json_dumps(info, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
	json_decref(info);
	return info_text;
}

struct janus_plugin_result *janus_source_handle_message(janus_plugin_session *handle, char *transaction, char *message, char *sdp_type, char *sdp) {
	if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return janus_plugin_result_new(JANUS_PLUGIN_ERROR, g_atomic_int_get(&stopping) ? "Shutting down" : "Plugin not initialized");
	janus_source_message *msg = g_malloc0(sizeof(janus_source_message));
	if (msg == NULL) {
		JANUS_LOG(LOG_FATAL, "Memory error!\n");
		return janus_plugin_result_new(JANUS_PLUGIN_ERROR, "Memory error");
	}
	msg->handle = handle;
	msg->transaction = transaction;
	msg->message = message;
	msg->sdp_type = sdp_type;
	msg->sdp = sdp;
	g_async_queue_push(messages, msg);

	/* All the requests to this plugin are handled asynchronously */
	return janus_plugin_result_new(JANUS_PLUGIN_OK_WAIT, "I'm taking my time!");
}

void janus_source_setup_media(janus_plugin_session *handle) {
	JANUS_LOG(LOG_INFO, "WebRTC media is now available\n");
	if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_source_session *session = (janus_source_session *)handle->plugin_handle;
	if (!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if (session->destroyed)
		return;
	g_atomic_int_set(&session->hangingup, 0);
	/* We really don't care, as we only send RTP/RTCP we get in the first place back anyway */
	
	JANUS_LOG(LOG_VERB, "video_active: %d, audio_active: %d\n", 
		session->video_active, session->audio_active);
	
	QueueEventData *queue_event_data;
	queue_event_data = g_malloc0(sizeof(QueueEventData));
	queue_event_data->callback = rtsp_callback;
	queue_event_data->session = session;

	g_async_queue_push(rtsp_server_data->rtsp_async_queue, queue_event_data) ;
	g_main_context_wakeup(NULL) ;

#ifdef PLI_WORKAROUND
	if (pli_period > 0) {
		session->periodic_pli = g_timeout_add(pli_period, request_key_frame_periodic_cb, (gpointer)session);
	}
#endif
}

void janus_source_incoming_rtp(janus_plugin_session *handle, int video, char *buf, int len) {
	if (handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	/* Simple source plugin */
	if (gateway) {
		/* Honour the audio/video active flags */
		janus_source_session *session = (janus_source_session *)handle->plugin_handle;
		if (!session) {
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			return;
		}
		if (session->destroyed)
			return;
		if ((!video && session->audio_active) || (video && session->video_active)) {
			/* Save the frame if we're recording */
			janus_recorder_save_frame(video ? session->vrc : session->arc, buf, len);
			/* Send the frame back */
			//gateway->relay_rtp(handle, video, buf, len);
			janus_source_relay_rtp(session, video, buf, len);
		}
	}
}

void janus_source_incoming_rtcp(janus_plugin_session *handle, int video, char *buf, int len) {
	if (handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	/* Simple source plugin */
	if (gateway) {
		janus_source_session *session = (janus_source_session *)handle->plugin_handle;
		if (!session) {
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			return;
		}
		if (session->destroyed)
			return;
		//if (session->bitrate > 0)
		//	janus_rtcp_cap_remb(buf, len, session->bitrate);
		JANUS_LOG(LOG_HUGE, "%s RTCP received; len=%d\n", video ? "Video" : "Audio", len);
		janus_source_relay_rtcp(session, video, buf, len);
	}
}

void janus_source_incoming_data(janus_plugin_session *handle, char *buf, int len) {
	if (handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	/* Simple source plugin */
	if (gateway) {
		janus_source_session *session = (janus_source_session *)handle->plugin_handle;
		if (!session) {
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			return;
		}
		if (session->destroyed)
			return;
		if (buf == NULL || len <= 0)
			return;
		JANUS_LOG(LOG_VERB, "Ignoring DataChannel message (%d bytes)\n", len);
	}
}

void janus_source_slow_link(janus_plugin_session *handle, int uplink, int video) {
	/* The core is informing us that our peer got or sent too many NACKs, are we pushing media too hard? */
	if (handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_source_session *session = (janus_source_session *)handle->plugin_handle;
	if (!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if (session->destroyed)
		return;
	session->slowlink_count++;
	if (uplink && !video && !session->audio_active) {
		/* We're not relaying audio and the peer is expecting it, so NACKs are normal */
		JANUS_LOG(LOG_VERB, "Getting a lot of NACKs (slow uplink) for audio, but that's expected, a configure disabled the audio forwarding\n");
	}
	else if (uplink && video && !session->video_active) {
		/* We're not relaying video and the peer is expecting it, so NACKs are normal */
		JANUS_LOG(LOG_VERB, "Getting a lot of NACKs (slow uplink) for video, but that's expected, a configure disabled the video forwarding\n");
	}
	else {
		/* Slow uplink or downlink, maybe we set the bitrate cap too high? */
		if (video) {
			/* Halve the bitrate, but don't go too low... */
			session->bitrate = session->bitrate > 0 ? session->bitrate : 512 * 1024;
			session->bitrate = session->bitrate / 2;
			if (session->bitrate < 64 * 1024)
				session->bitrate = 64 * 1024;
			JANUS_LOG(LOG_WARN, "Getting a lot of NACKs (slow %s) for %s, forcing a lower REMB: %"SCNu64"\n",
				uplink ? "uplink" : "downlink", video ? "video" : "audio", session->bitrate);
			/* ... and send a new REMB back */
			char rtcpbuf[24];
			janus_rtcp_remb((char *)(&rtcpbuf), 24, session->bitrate);
			gateway->relay_rtcp(handle, 1, rtcpbuf, 24);
			/* As a last thing, notify the user about this */
			json_t *event = json_object();
			json_object_set_new(event, "source", json_string("event"));
			json_t *result = json_object();
			json_object_set_new(result, "status", json_string("slow_link"));
			json_object_set_new(result, "bitrate", json_integer(session->bitrate));
			json_object_set_new(event, "result", result);
			char *event_text = json_dumps(event, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
			json_decref(event);
			json_decref(result);
			event = NULL;
			gateway->push_event(session->handle, &janus_source_plugin, NULL, event_text, NULL, NULL);
			g_free(event_text);
		}
	}
}

void janus_source_hangup_media(janus_plugin_session *handle) {
	JANUS_LOG(LOG_INFO, "No WebRTC media anymore\n");
	if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_source_session *session = (janus_source_session *)handle->plugin_handle;
	if (!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if (session->destroyed)
		return;
	if (g_atomic_int_add(&session->hangingup, 1))
		return;
	/* Send an event to the browser and tell it's over */
	json_t *event = json_object();
	json_object_set_new(event, "source", json_string("event"));
	json_object_set_new(event, "result", json_string("done"));
	char *event_text = json_dumps(event, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
	json_decref(event);
	JANUS_LOG(LOG_VERB, "Pushing event: %s\n", event_text);
	int ret = gateway->push_event(handle, &janus_source_plugin, NULL, event_text, NULL, NULL);
	JANUS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
	g_free(event_text);
	/* Get rid of the recorders, if available */
	janus_mutex_lock(&session->rec_mutex);
	if (session->arc) {
		janus_recorder_close(session->arc);
		JANUS_LOG(LOG_INFO, "Closed audio recording %s\n", session->arc->filename ? session->arc->filename : "??");
		janus_recorder_free(session->arc);
	}
	session->arc = NULL;
	if (session->vrc) {
		janus_recorder_close(session->vrc);
		JANUS_LOG(LOG_INFO, "Closed video recording %s\n", session->vrc->filename ? session->vrc->filename : "??");
		janus_recorder_free(session->vrc);
	}
	session->vrc = NULL;
	janus_mutex_unlock(&session->rec_mutex);
	/* Reset controls */
	session->audio_active = TRUE;
	session->video_active = TRUE;
	session->bitrate = 0;
}

/* Thread to handle incoming messages */
static void *janus_source_handler(void *data) {
	JANUS_LOG(LOG_VERB, "Joining SourcePlugin handler thread\n");
	janus_source_message *msg = NULL;
	int error_code = 0;
	char *error_cause = g_malloc0(512);
	if (error_cause == NULL) {
		JANUS_LOG(LOG_FATAL, "Memory error!\n");
		return NULL;
	}
	json_t *root = NULL;
	while (g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
		msg = g_async_queue_pop(messages);
		if (msg == NULL)
			continue;
		if (msg == &exit_message)
			break;
		if (msg->handle == NULL) {
			janus_source_message_free(msg);
			continue;
		}
		janus_source_session *session = NULL;
		janus_mutex_lock(&sessions_mutex);
		if (g_hash_table_lookup(sessions, msg->handle) != NULL) {
			session = (janus_source_session *)msg->handle->plugin_handle;
		}
		janus_mutex_unlock(&sessions_mutex);
		if (!session) {
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			janus_source_message_free(msg);
			continue;
		}
		if (session->destroyed) {
			janus_source_message_free(msg);
			continue;
		}
		/* Handle request */
		error_code = 0;
		root = NULL;
		JANUS_LOG(LOG_VERB, "Handling message: %s\n", msg->message);
		if (msg->message == NULL) {
			JANUS_LOG(LOG_ERR, "No message??\n");
			error_code = JANUS_SOURCE_ERROR_NO_MESSAGE;
			g_snprintf(error_cause, 512, "%s", "No message??");
			goto error;
		}
		json_error_t error;
		root = json_loads(msg->message, 0, &error);
		if (!root) {
			JANUS_LOG(LOG_ERR, "JSON error: on line %d: %s\n", error.line, error.text);
			error_code = JANUS_SOURCE_ERROR_INVALID_JSON;
			g_snprintf(error_cause, 512, "JSON error: on line %d: %s", error.line, error.text);
			goto error;
		}
		if (!json_is_object(root)) {
			JANUS_LOG(LOG_ERR, "JSON error: not an object\n");
			error_code = JANUS_SOURCE_ERROR_INVALID_JSON;
			g_snprintf(error_cause, 512, "JSON error: not an object");
			goto error;
		}
		/* Parse request */
		json_t *audio = json_object_get(root, "audio");
		if (audio && !json_is_boolean(audio)) {
			JANUS_LOG(LOG_ERR, "Invalid element (audio should be a boolean)\n");
			error_code = JANUS_SOURCE_ERROR_INVALID_ELEMENT;
			g_snprintf(error_cause, 512, "Invalid value (audio should be a boolean)");
			goto error;
		}
		json_t *video = json_object_get(root, "video");
		if (video && !json_is_boolean(video)) {
			JANUS_LOG(LOG_ERR, "Invalid element (video should be a boolean)\n");
			error_code = JANUS_SOURCE_ERROR_INVALID_ELEMENT;
			g_snprintf(error_cause, 512, "Invalid value (video should be a boolean)");
			goto error;
		}
		json_t *bitrate = json_object_get(root, "bitrate");
		if (bitrate && (!json_is_integer(bitrate) || json_integer_value(bitrate) < 0)) {
			JANUS_LOG(LOG_ERR, "Invalid element (bitrate should be a positive integer)\n");
			error_code = JANUS_SOURCE_ERROR_INVALID_ELEMENT;
			g_snprintf(error_cause, 512, "Invalid value (bitrate should be a positive integer)");
			goto error;
		}
		json_t *record = json_object_get(root, "record");
		if (record && !json_is_boolean(record)) {
			JANUS_LOG(LOG_ERR, "Invalid element (record should be a boolean)\n");
			error_code = JANUS_SOURCE_ERROR_INVALID_ELEMENT;
			g_snprintf(error_cause, 512, "Invalid value (record should be a boolean)");
			goto error;
		}
		json_t *recfile = json_object_get(root, "filename");
		if (recfile && !json_is_string(recfile)) {
			JANUS_LOG(LOG_ERR, "Invalid element (filename should be a string)\n");
			error_code = JANUS_SOURCE_ERROR_INVALID_ELEMENT;
			g_snprintf(error_cause, 512, "Invalid value (filename should be a string)");
			goto error;
		}
		/* Enforce request */
		if (audio) {
			session->audio_active = json_is_true(audio);
			JANUS_LOG(LOG_VERB, "Setting audio property: %s\n", session->audio_active ? "true" : "false");
		}
		if (video) {
			if (!session->video_active && json_is_true(video)) {
				/* Send a PLI */
				JANUS_LOG(LOG_VERB, "Just (re-)enabled video, sending a PLI to recover it\n");
				char buf[12];
				memset(buf, 0, 12);
				janus_rtcp_pli((char *)&buf, 12);
				gateway->relay_rtcp(session->handle, 1, buf, 12);
			}
			session->video_active = json_is_true(video);
			JANUS_LOG(LOG_VERB, "Setting video property: %s\n", session->video_active ? "true" : "false");
		}
		if (bitrate) {
			session->bitrate = json_integer_value(bitrate);
			JANUS_LOG(LOG_VERB, "Setting video bitrate: %"SCNu64"\n", session->bitrate);
			if (session->bitrate > 0) {
				/* FIXME Generate a new REMB (especially useful for Firefox, which doesn't send any we can cap later) */
				char buf[24];
				memset(buf, 0, 24);
				janus_rtcp_remb((char *)&buf, 24, session->bitrate);
				JANUS_LOG(LOG_VERB, "Sending REMB\n");
				gateway->relay_rtcp(session->handle, 1, buf, 24);
				/* FIXME How should we handle a subsequent "no limit" bitrate? */
			}
		}
#if 0 //recording is not supported; leaving it for future
		if (record) {
			if (msg->sdp) {
				session->has_audio = (strstr(msg->sdp, "m=audio") != NULL);
				session->has_video = (strstr(msg->sdp, "m=video") != NULL);
			}
			gboolean recording = json_is_true(record);
			const char *recording_base = json_string_value(recfile);
			JANUS_LOG(LOG_VERB, "Recording %s (base filename: %s)\n", recording ? "enabled" : "disabled", recording_base ? recording_base : "not provided");
			janus_mutex_lock(&session->rec_mutex);
			if (!recording) {
				/* Not recording (anymore?) */
				if (session->arc) {
					janus_recorder_close(session->arc);
					JANUS_LOG(LOG_INFO, "Closed audio recording %s\n", session->arc->filename ? session->arc->filename : "??");
					janus_recorder_free(session->arc);
				}
				session->arc = NULL;
				if (session->vrc) {
					janus_recorder_close(session->vrc);
					JANUS_LOG(LOG_INFO, "Closed video recording %s\n", session->vrc->filename ? session->vrc->filename : "??");
					janus_recorder_free(session->vrc);
				}
				session->vrc = NULL;
			}
			else {
				/* We've started recording, send a PLI and go on */
				char filename[255];
				gint64 now = janus_get_real_time();
				if (session->has_audio) {
					/* FIXME We assume we're recording Opus, here */
					memset(filename, 0, 255);
					if (recording_base) {
						/* Use the filename and path we have been provided */
						g_snprintf(filename, 255, "%s-audio", recording_base);
						session->arc = janus_recorder_create(NULL, "opus", filename);
						if (session->arc == NULL) {
							/* FIXME We should notify the fact the recorder could not be created */
							JANUS_LOG(LOG_ERR, "Couldn't open an audio recording file for this SourcePlugin user!\n");
						}
					}
					else {
						/* Build a filename */
						g_snprintf(filename, 255, "source-%p-%"SCNi64"-audio", session, now);
						session->arc = janus_recorder_create(NULL, "opus", filename);
						if (session->arc == NULL) {
							/* FIXME We should notify the fact the recorder could not be created */
							JANUS_LOG(LOG_ERR, "Couldn't open an audio recording file for this SourcePlugin user!\n");
						}
					}
				}
				if (session->has_video) {
					/* FIXME We assume we're recording VP8, here */
					memset(filename, 0, 255);
					if (recording_base) {
						/* Use the filename and path we have been provided */
						g_snprintf(filename, 255, "%s-video", recording_base);
						session->vrc = janus_recorder_create(NULL, "vp8", filename);
						if (session->vrc == NULL) {
							/* FIXME We should notify the fact the recorder could not be created */
							JANUS_LOG(LOG_ERR, "Couldn't open an video recording file for this SourcePlugin user!\n");
						}
					}
					else {
						/* Build a filename */
						g_snprintf(filename, 255, "source-%p-%"SCNi64"-video", session, now);
						session->vrc = janus_recorder_create(NULL, "vp8", filename);
						if (session->vrc == NULL) {
							/* FIXME We should notify the fact the recorder could not be created */
							JANUS_LOG(LOG_ERR, "Couldn't open an video recording file for this SourcePlugin user!\n");
						}
					}
					/* Send a PLI */
					JANUS_LOG(LOG_VERB, "Recording video, sending a PLI to kickstart it\n");
					char buf[12];
					memset(buf, 0, 12);
					janus_rtcp_pli((char *)&buf, 12);
					gateway->relay_rtcp(session->handle, 1, buf, 12);
				}
			}
			janus_mutex_unlock(&session->rec_mutex);
		}
#endif //recording
		
		if (!audio && !video && !bitrate && !record && !msg->sdp) {
			JANUS_LOG(LOG_ERR, "No supported attributes (audio, video, bitrate, record, jsep) found\n");
			error_code = JANUS_SOURCE_ERROR_INVALID_ELEMENT;
			g_snprintf(error_cause, 512, "Message error: no supported attributes (audio, video, bitrate, record, jsep) found");
			goto error;
		}

		json_decref(root);
		/* Prepare JSON event */
		json_t *event = json_object();
		json_object_set_new(event, "source", json_string("event"));
		json_object_set_new(event, "result", json_string("ok"));
		char *event_text = json_dumps(event, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
		json_decref(event);
		JANUS_LOG(LOG_VERB, "Pushing event: %s\n", event_text);
		if (!msg->sdp) {
			int ret = gateway->push_event(msg->handle, &janus_source_plugin, msg->transaction, event_text, NULL, NULL);
			JANUS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
		}
		else {
			/* Forward the same offer to the gateway, to start the source plugin */
			const char *type = NULL;
			if (!strcasecmp(msg->sdp_type, "offer"))
				type = "answer";
			if (!strcasecmp(msg->sdp_type, "answer"))
				type = "offer";
			/* Any media direction that needs to be fixed? */
			char *sdp = g_strdup(msg->sdp);
			if (strstr(sdp, "a=recvonly")) {
				/* Turn recvonly to inactive, as we simply bounce media back */
				sdp = janus_string_replace(sdp, "a=recvonly", "a=inactive");
			}
			else if (strstr(sdp, "a=sendonly")) {
				/* Turn sendonly to recvonly */
				sdp = janus_string_replace(sdp, "a=sendonly", "a=recvonly");
				/* FIXME We should also actually not echo this media back, though... */
			}
			/* Make also sure we get rid of ULPfec, red, etc. */
			if (strstr(sdp, "ulpfec")) {
				/* FIXME This really needs some better code */
				sdp = janus_string_replace(sdp, "a=rtpmap:116 red/90000\r\n", "");
				sdp = janus_string_replace(sdp, "a=rtpmap:117 ulpfec/90000\r\n", "");
				sdp = janus_string_replace(sdp, "a=rtpmap:96 rtx/90000\r\n", "");
				sdp = janus_string_replace(sdp, "a=fmtp:96 apt=100\r\n", "");
				sdp = janus_string_replace(sdp, "a=rtpmap:97 rtx/90000\r\n", "");
				sdp = janus_string_replace(sdp, "a=fmtp:97 apt=101\r\n", "");
				sdp = janus_string_replace(sdp, "a=rtpmap:98 rtx/90000\r\n", "");
				sdp = janus_string_replace(sdp, "a=fmtp:98 apt=116\r\n", "");
				sdp = janus_string_replace(sdp, " 116", "");
				sdp = janus_string_replace(sdp, " 117", "");
				sdp = janus_string_replace(sdp, " 96", "");
				sdp = janus_string_replace(sdp, " 97", "");
				sdp = janus_string_replace(sdp, " 98", "");
			}
			
			sdp = janus_source_do_codec_negotiation(session, sdp);
			
			/* How long will the gateway take to push the event? */
			g_atomic_int_set(&session->hangingup, 0);
			gint64 start = janus_get_monotonic_time();
			int res = gateway->push_event(msg->handle, &janus_source_plugin, msg->transaction, event_text, type, sdp);
			JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (took %"SCNu64" us)\n",
				res, janus_get_monotonic_time() - start);
			g_free(sdp);
		}
		g_free(event_text);
		janus_source_message_free(msg);
		continue;

	error:
		{
			if (root != NULL)
				json_decref(root);
			/* Prepare JSON error event */
			json_t *event = json_object();
			json_object_set_new(event, "source", json_string("event"));
			json_object_set_new(event, "error_code", json_integer(error_code));
			json_object_set_new(event, "error", json_string(error_cause));
			char *event_text = json_dumps(event, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
			json_decref(event);
			JANUS_LOG(LOG_VERB, "Pushing event: %s\n", event_text);
			int ret = gateway->push_event(msg->handle, &janus_source_plugin, msg->transaction, event_text, NULL, NULL);
			JANUS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
			g_free(event_text);
			janus_source_message_free(msg);
		}
	}
	g_free(error_cause);
	JANUS_LOG(LOG_VERB, "Leaving SourcePlugin handler thread\n");
	return NULL;
}

void janus_source_relay_rtp(janus_source_session *session, int video, char *buf, int len) {
	GSocket * sock_rtp_cli = video ? session->socket[JANUS_SOURCE_STREAM_VIDEO][JANUS_SOURCE_SOCKET_RTP_CLI].socket :  session->socket[JANUS_SOURCE_STREAM_AUDIO][JANUS_SOURCE_SOCKET_RTP_CLI].socket;

	if (!sock_rtp_cli)
		return;

	if (g_socket_send(sock_rtp_cli, buf, len, NULL, NULL) < 0) {
		//JANUS_LOG(LOG_ERR, "Send RTP failed! type: %s\n", video ? "video" : "audio");
	}
	else {
		//JANUS_LOG(LOG_ERR, "Send RTP successfully! type: %s; len=%d\n", video ? "video" : "audio", len);
	}

}

static void janus_source_relay_rtcp(janus_source_session *session, int video, char *buf, int len) {

	GSocket * sock_rtcp_cli = video ? session->socket[JANUS_SOURCE_STREAM_VIDEO][JANUS_SOURCE_SOCKET_RTCP_RCV_CLI].socket :  session->socket[JANUS_SOURCE_STREAM_AUDIO][JANUS_SOURCE_SOCKET_RTCP_RCV_CLI].socket;

	if (!sock_rtcp_cli)
		return;

	if (g_socket_send(sock_rtcp_cli, buf, len, NULL, NULL) < 0) {
		//JANUS_LOG(LOG_ERR, "Send RTCP failed! type: %s\n", video ? "video" : "audio");
	}
	else {
		//JANUS_LOG(LOG_ERR, "Send RTCP successfully! type: %s; len=%d\n", video ? "video" : "audio", len);
	}
}


static void rtsp_media_target_state_cb(GstRTSPMedia *gstrtspmedia, gint state, gpointer data)
{
	JANUS_LOG(LOG_INFO, "rtsp_media_target_state_cb: %d\n", (GstState)state);
	janus_source_session * session = (janus_source_session *)data;
	
	if (!session) {
		JANUS_LOG(LOG_ERR, "rtsp_media_target_state_cb: session is NULL\n");
		return;
	}

#ifdef PLI_WORKAROUND

	g_atomic_int_set(&session->rtsp_state, state);
	
	if (state == GST_STATE_PAUSED) {
		janus_source_request_keyframe(data);
		/* ensure after 2s if keyframe was really obtained and pipeline switched into PLAYING */
		g_timeout_add(2000, request_key_frame_if_not_playing_cb, data); 
	}
#endif

	if (state == GST_STATE_PAUSED) {
		GstElement * bin = NULL;
		GSocket * socket = NULL;

		bin = gst_rtsp_media_get_element(gstrtspmedia);
		g_assert(bin);
		
		for (int stream = 0; stream < JANUS_SOURCE_STREAM_MAX; stream++)
		{
			GstElement * udp_src_rtp = gst_bin_get_by_name(GST_BIN(bin), janus_source_get_udpsrc_name(stream, JANUS_SOURCE_SOCKET_RTP_SRV));
			g_assert(udp_src_rtp);
		
			socket = session->socket[stream][JANUS_SOURCE_SOCKET_RTP_SRV].socket;
			g_assert(socket);
			g_object_set(udp_src_rtp, "socket", socket, NULL);
			g_object_unref(udp_src_rtp);
		
			GstElement * udpsrc_rtcp_receive = gst_bin_get_by_name(GST_BIN(bin), janus_source_get_udpsrc_name(stream, JANUS_SOURCE_SOCKET_RTCP_RCV_SRV));
			g_assert(udpsrc_rtcp_receive);
		
			socket = session->socket[stream][JANUS_SOURCE_SOCKET_RTCP_RCV_SRV].socket;
			g_assert(socket);
			g_object_set(udpsrc_rtcp_receive, "socket", socket, NULL);
			g_object_unref(udpsrc_rtcp_receive);
		}
		g_object_unref(bin);
	}
}

static void media_configure_cb(GstRTSPMediaFactory * factory, GstRTSPMedia * media, gpointer data)
{
	JANUS_LOG(LOG_INFO, "media_configure callback\n") ;
	g_signal_connect(media, "target-state", (GCallback)rtsp_media_target_state_cb, data);
}

static void
client_play_request_cb(GstRTSPClient  *gstrtspclient,
	GstRTSPContext *rtspcontext,
	gpointer        data)
{
	JANUS_LOG(LOG_INFO, "client_play_request_cb\n");
#ifdef PLI_WORKAROUND
	//give clients some time to start collecting data
	g_timeout_add(1000, request_key_frame_cb, data) ;
#endif
}

static void
client_connected_cb(GstRTSPServer *gstrtspserver,
	GstRTSPClient *gstrtspclient,
	gpointer       data)
{
	GstRTSPClientClass *klass = GST_RTSP_CLIENT_GET_CLASS(gstrtspclient);
	klass->create_sdp = create_sdp;
	JANUS_LOG(LOG_INFO, "New client connected\n");		
	g_signal_connect(gstrtspclient, "play-request", (GCallback)client_play_request_cb, data);
}

static GstRTSPFilterResult
janus_source_close_rtsp_sessions(GstRTSPSessionPool *pool, GstRTSPSession *session, gpointer data) { 
	JANUS_LOG(LOG_INFO, "Removing RTSP session: %s\n", gst_rtsp_session_get_sessionid(session));
	return GST_RTSP_FILTER_REMOVE;	
}

//todo: fix
static const gchar * janus_source_get_udpsrc_name(int stream, int type) { 
	switch(stream)
	{
		case JANUS_SOURCE_STREAM_VIDEO :
			switch (type)
			{
			case JANUS_SOURCE_SOCKET_RTP_SRV:
				return "udpsrc_rtp_video";
			case JANUS_SOURCE_SOCKET_RTCP_RCV_SRV:
				return "udpsrc_rtcp_receive_video";
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
				return "udpsrc_rtcp_receive_audio";
			default:
				break;
			}
			break ;
		default:
			break;
	 }
	return NULL;
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
				janus_source_get_udpsrc_name(stream, JANUS_SOURCE_SOCKET_RTP_SRV),
				session->codec_pt[stream],
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

static gboolean janus_source_create_sockets(janus_source_socket socket[JANUS_SOURCE_STREAM_MAX][JANUS_SOURCE_SOCKET_MAX]) {
	
	gboolean result = TRUE;

	for (int i = 0; i < JANUS_SOURCE_STREAM_MAX; i++)
	{
		if (!socket_utils_create_server_socket(&socket[i][JANUS_SOURCE_SOCKET_RTP_SRV])) {
			result = FALSE;
		}

		if (!socket_utils_create_client_socket(&socket[i][JANUS_SOURCE_SOCKET_RTP_CLI], socket[i][JANUS_SOURCE_SOCKET_RTP_SRV].port)) {
			result = FALSE;
		}
	
		if (!socket_utils_create_server_socket(&socket[i][JANUS_SOURCE_SOCKET_RTCP_RCV_SRV])) {
			result = FALSE;
		}

		if (!socket_utils_create_client_socket(&socket[i][JANUS_SOURCE_SOCKET_RTCP_RCV_CLI], socket[i][JANUS_SOURCE_SOCKET_RTCP_RCV_SRV].port)) {
			result = FALSE;
		}
	
		if (!socket_utils_create_server_socket(&socket[i][JANUS_SOURCE_SOCKET_RTCP_SND_SRV])) {
			result = FALSE;
		}
	}
	
	return result;
}


static void *janus_source_rtsp_server_thread(void *data) {

	GMainLoop *loop;
	GstRTSPSessionPool *session_pool;
	GList * sessions_list;	

	if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		JANUS_LOG(LOG_INFO, "Plugin is stopping\n");
		return FALSE;
	}

	/*Create rtsp server and async queue*/
	rtsp_server_data = g_malloc0(sizeof(janus_source_rtsp_server_data));
	//rtsp_server_data->rtsp_server = gst_rtsp_server_new();
	//rtsp_server_data->rtsp_async_queue =  g_async_queue_new();
	create_rtsp_server_and_queue(rtsp_server_data);

#ifdef USE_THREAD_CONTEXT
	/* Set up a worker context and make it thread-default */
	GMainContext *worker_context = g_main_context_new();
	g_main_context_push_thread_default(worker_context);
#endif
	/* Create new queue source */
	janus_source_attach_rtsp_queue_callback(rtsp_server_data, queue_events_callback, g_main_context_get_thread_default());
	
	/* make a mainloop for the thread-default context */
	loop = g_main_loop_new(g_main_context_get_thread_default(), FALSE);
	rtsp_server_data->loop = loop;
	g_main_loop_run(loop);
	

	session_pool = gst_rtsp_server_get_session_pool(rtsp_server_data->rtsp_server);
	sessions_list = gst_rtsp_session_pool_filter(session_pool, janus_source_close_rtsp_sessions, NULL);
	g_list_free_full(sessions_list, gst_object_unref);
	g_object_unref(session_pool);

#ifdef USE_THREAD_CONTEXT
	g_main_context_pop_thread_default(worker_context);
	g_main_context_unref(worker_context);
#endif

	return NULL;
}

#ifdef PLI_WORKAROUND
static void janus_source_request_keyframe(janus_source_session *session)
{

	if (!session) {
		JANUS_LOG(LOG_ERR, "keyframe_once_cb: session is NULL\n");
		return;
	}

	if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized) || g_atomic_int_get(&session->hangingup) || session->destroyed) {
		JANUS_LOG(LOG_VERB, "Keyframe generation event while plugin or session is stopping\n");
		return;
	}

	JANUS_LOG(LOG_INFO, "Sending a PLI to request keyframe\n");
	char buf[12];
	memset(buf, 0, 12);
	janus_rtcp_pli((char *)&buf, 12);
	gateway->relay_rtcp(session->handle, 1, buf, 12);
}

static gboolean
request_key_frame_cb(gpointer data)
{
	janus_source_request_keyframe((janus_source_session *)data);
	return FALSE;
}

static gboolean
request_key_frame_periodic_cb(gpointer data)
{
	janus_source_request_keyframe((janus_source_session *)data);
	return TRUE;
}

static gboolean
request_key_frame_if_not_playing_cb(gpointer data)
{
	janus_source_session * session = (janus_source_session *)data;

	if (!session) {
	JANUS_LOG(LOG_ERR, "keyframe_once_cb: session is NULL\n");
	return FALSE;
}

	if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized) || g_atomic_int_get(&session->hangingup) || session->destroyed) {
	JANUS_LOG(LOG_VERB, "Keyframe generation event while plugin or session is stopping\n");
	return FALSE;
}

	if (g_atomic_int_get(&session->rtsp_state) == GST_STATE_PAUSED) {
	JANUS_LOG(LOG_INFO, "State is GST_STATE_PAUSED; sendig pli and schedulling check\n");
	janus_source_request_keyframe(data);
	/* call this callback again, as we are still in PAUSED state */
	return TRUE;
}

	return FALSE;
}
#endif

static gboolean janus_source_send_rtcp_src_received(GSocket *socket, GIOCondition condition, janus_source_rtcp_cbk_data * data)
{
	char buf[512];
	gssize len;
	janus_source_session * session = (janus_source_session*)data->session;

	if (!session) {
		JANUS_LOG(LOG_ERR, "janus_source_send_rtcp_src_received: session is NULL\n");
		return TRUE;
	}

	len = g_socket_receive(socket, (gchar*)buf, sizeof(buf), NULL, NULL);

	if (len > 0) {
		JANUS_LOG(LOG_HUGE, "%s RTCP sent; len=%ld\n", data->is_video ? "Video" : "Audio", len);
		gateway->relay_rtcp(session->handle, data->is_video, buf, len);
	}

	return TRUE;
}



static void janus_source_close_session_func(gpointer key, gpointer value, gpointer user_data) {

	if (value != NULL) {
		janus_source_close_session((janus_source_session *)value);
	}
}

static void janus_source_close_session(janus_source_session * session) {
	JANUS_LOG(LOG_INFO, "Closing source session\n");

	gchar *session_id = g_strdup(session->db_entry_session_id);
	gchar *curl_str = g_strdup_printf("%s/%s", status_service_url, session_id);	

	gboolean retCode = curl_request(curl_handle, curl_str, "{}", "DELETE", NULL, FALSE);		    
	
	if (retCode != TRUE) {
	    JANUS_LOG(LOG_ERR, "Could not send the request to the server\n"); 
	}
			
	if (session_id) {
		g_free(session_id);
	}
	
	if (curl_str) {
		g_free(curl_str);
	}

	for (int stream = 0; stream < JANUS_SOURCE_STREAM_MAX; stream++)
	{
		for (int j = 0; j < JANUS_SOURCE_SOCKET_MAX; j++)
		{
			socket_utils_close_socket(&session->socket[stream][j]);
		}
	}

#ifdef PLI_WORKAROUND
	if (session->periodic_pli) {
		g_source_remove(session->periodic_pli);
	}
#endif

	if (session->db_entry_session_id != NULL) {
		g_free(session->db_entry_session_id);
	    session->db_entry_session_id = NULL;
	}
}

static void janus_source_parse_ports_range(janus_config_item *ports_range, uint16_t *min_port, uint16_t * max_port)
{
	if (ports_range && ports_range->value)
	{
		/* Split in min and max port */
		char *maxport = strrchr(ports_range->value, '-');
		if (maxport != NULL)
		{
			*maxport = '\0';
			maxport++;
			*min_port = atoi(ports_range->value);
			*max_port = atoi(maxport);
			maxport--;
			*maxport = '-';
		}
		if (*min_port > *max_port)
		{
			int temp_port = *min_port;
			*min_port = *max_port;
			*max_port = temp_port;
		}
		if (*max_port == 0)
			*max_port = 65535;
		JANUS_LOG(LOG_INFO, "UDP port range: %u - %u\n", *min_port, *max_port);
	}
}


static void janus_source_parse_status_service_url(janus_config_item *config_url, gchar **url) {
    if(config_url && config_url->value){ 
		*url = g_strdup(config_url->value);
    }
}

static void janus_source_parse_pli_period(janus_config_item *config_pli, gint *pli_period) {
	if (config_pli && config_pli->value) {
		*pli_period = atoi(config_pli->value);
	} else {
		*pli_period = 0;
	}
}

static void janus_source_parse_video_codec_priority(janus_config_item *config)
{
	if (config && config->value)
	{
		char * codec2 = strrchr(config->value, ',');
		if (codec2 != NULL)
		{
			*codec2 = '\0';
			codec2++;
			const char * codec1 = config->value;
			
			codec_priority_list[0] = sdp_codec_name_to_id(codec1);
			codec_priority_list[1] = sdp_codec_name_to_id(codec2);
			use_codec_priority = TRUE;
		}
	}
	else
	{
		use_codec_priority = FALSE;
	}
}

static GstSDPMessage *
create_sdp(GstRTSPClient * client, GstRTSPMedia * media)
{
	GstSDPMessage *sdp;
	GstSDPInfo info;

	guint64 session_id_tmp;
	gchar session_id[21];
	const char * server_ip = janus_get_local_ip();
	const gchar *proto = "IP4"; //todo: support IPV6

	gst_sdp_message_new(&sdp);

	/* some standard things first */
	gst_sdp_message_set_version(sdp, "0");

	session_id_tmp = (((guint64)g_random_int()) << 32) | g_random_int();
	g_snprintf(session_id, sizeof(session_id), "%" G_GUINT64_FORMAT,
		session_id_tmp);

	gst_sdp_message_set_origin(sdp, "-", session_id, "1", "IN", proto, server_ip);

	gst_sdp_message_set_session_name(sdp, "Idilia source session");
	gst_sdp_message_set_information(sdp, "rtsp-server");
	gst_sdp_message_add_time(sdp, "0", "0", NULL);
	gst_sdp_message_add_attribute(sdp, "tool", "GStreamer");
	gst_sdp_message_add_attribute(sdp, "type", "broadcast");
	gst_sdp_message_add_attribute(sdp, "control", "*");

	info.is_ipv6 = !!g_strcmp0(proto, "IP4");
	info.server_ip = server_ip;

	/* create an SDP for the media object */
	if (!gst_rtsp_media_setup_sdp(media, sdp, &info))
		goto no_sdp;

	GstSDPMedia * sdpmedia = (GstSDPMedia *)gst_sdp_message_get_media(sdp, 0);
	g_assert(sdpmedia);

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

static idilia_codec janus_source_select_video_codec_by_priority_list(const gchar * sdp)
{
	for (guint i = 0; i < sizeof(codec_priority_list) / sizeof(codec_priority_list[0]); i++) {
	if (sdp_get_codec_pt(sdp, codec_priority_list[i]) != -1)
		return codec_priority_list[i];
	}
	
	return IDILIA_CODEC_INVALID;
}

static gchar * janus_source_do_codec_negotiation(janus_source_session * session, gchar * orig_sdp)
{
	gchar * sdp = NULL;
	
	idilia_codec preferred_codec = janus_source_select_video_codec_by_priority_list(orig_sdp);
	sdp = sdp_set_video_codec(orig_sdp, preferred_codec);
	g_free(orig_sdp);
	
	for (int stream = 0; stream < JANUS_SOURCE_STREAM_MAX; stream++)
	{
		if (stream == JANUS_SOURCE_STREAM_VIDEO) {
			session->codec[stream] = sdp_get_video_codec(sdp);
		}
		else if (stream == JANUS_SOURCE_STREAM_AUDIO) {
			session->codec[stream] = sdp_get_audio_codec(sdp);
		}
		
		session->codec_pt[stream] = sdp_get_codec_pt(sdp, session->codec[stream]);
	
		JANUS_LOG(LOG_INFO, "Codec used: %s\n", get_codec_name(session->codec[stream]));
	}
	return sdp;
}


static void rtsp_callback(gpointer data) {
	GstRTSPMountPoints *mounts;
	GstRTSPMediaFactory *factory;
	int rtsp_port;
	janus_source_session *session = (janus_source_session*)(data); 
	
	gchar * launch_pipe = NULL;

	gchar *http_post = g_strdup("POST");	
	
		if (!session) {
		JANUS_LOG(LOG_ERR, "Session is NULL\n");		 
	}

	if (g_atomic_int_get(&session->hangingup) || session->destroyed) {
		JANUS_LOG(LOG_INFO, "Session is being destroyed\n");		 
	}

	for (int i = 0; i < JANUS_SOURCE_STREAM_MAX; i++)
	{
		for (int j = 0; j < JANUS_SOURCE_SOCKET_MAX; j++)
			JANUS_LOG(LOG_VERB, "UDP port[%d][%d]: %d\n", i, j, session->socket[i][j].port);
	}

	gst_rtsp_server_set_address(rtsp_server_data->rtsp_server, janus_get_local_ip());

	/* Allocate random port */
	gst_rtsp_server_set_service(rtsp_server_data->rtsp_server, "0");

	factory = gst_rtsp_media_factory_new();

	gst_rtsp_media_factory_set_latency(factory, 0);

#if 0
	gst_rtsp_media_factory_set_profiles(factory, GST_RTSP_PROFILE_AVP);
#else
	gst_rtsp_media_factory_set_profiles(factory, GST_RTSP_PROFILE_AVPF);

	/* store up to 100ms of retransmission data */
	gst_rtsp_media_factory_set_retransmission_time(factory, 100 * GST_MSECOND);
#endif

	launch_pipe = janus_source_create_launch_pipe(session);

	gst_rtsp_media_factory_set_launch(factory, launch_pipe);
	g_free(launch_pipe);

	/* media created from this factory can be shared between clients */
	gst_rtsp_media_factory_set_shared(factory, TRUE);

	for (int stream = 0; stream < JANUS_SOURCE_STREAM_MAX; stream++)
	{
		session->rtcp_cbk_data[stream].session = (gpointer)session;
		session->rtcp_cbk_data[stream].is_video = (stream == JANUS_SOURCE_STREAM_VIDEO);

		socket_utils_attach_callback(&session->socket[stream][JANUS_SOURCE_SOCKET_RTCP_SND_SRV],
			(GSourceFunc)janus_source_send_rtcp_src_received,
			(gpointer)&session->rtcp_cbk_data[stream]);
	}

	g_signal_connect(factory, "media-configure", (GCallback)media_configure_cb,
		(gpointer)session);

	g_signal_connect(rtsp_server_data->rtsp_server, "client-connected", (GCallback)client_connected_cb,
		(gpointer)session);

	/* get the default mount points from the server */
	mounts = gst_rtsp_server_get_mount_points(rtsp_server_data->rtsp_server);

	/* attach the session to the "/camera" URL */
	gst_rtsp_mount_points_add_factory(mounts, "/camera", factory);
	g_object_unref(mounts);
	
	/* attach the server to the thread-default context */
	if (gst_rtsp_server_attach(rtsp_server_data->rtsp_server, g_main_context_get_thread_default()) == 0) {
		JANUS_LOG(LOG_ERR, "Failed to attach the server\n");
	}
	
	rtsp_port = gst_rtsp_server_get_bound_port(rtsp_server_data->rtsp_server);

	gchar *rtsp_url = g_strdup_printf("rtsp://%s:%d/camera", janus_get_local_ip(), rtsp_port);

	gboolean retCode = curl_request(curl_handle, status_service_url, rtsp_url, http_post, &(session->db_entry_session_id), TRUE);
	if (retCode != TRUE) {
		JANUS_LOG(LOG_ERR, "Could not send the request to the server\n");
	}

	JANUS_LOG(LOG_INFO, "Stream ready at %s\n", rtsp_url);
	g_free(rtsp_url);
	g_free(http_post);
}

