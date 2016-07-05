/*! \file   idilia_source.c
* \author Lorenzo Miniero <lorenzo@meetecho.com>
*         Tomasz Zajac <tomasz.zajac@motorolasolutions.com>
* \copyright GNU General Public License v3
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
"source" : "event",
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
#include "ports_pool.h"
#include "node_service_access.h"


/* Plugin information */
#define JANUS_SOURCE_VERSION			1
#define JANUS_SOURCE_VERSION_STRING	    "0.0.1"
#define JANUS_SOURCE_DESCRIPTION		"Idilia source plugin"
#define JANUS_SOURCE_NAME		        "Idilia Source plugin"
#define JANUS_SOURCE_AUTHOR			    "Motorola Solutions Inc."
#define JANUS_SOURCE_PACKAGE			"idilia.plugin.source"

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

typedef struct janus_source_socket {
	int port;
	GSocket *socket;
} janus_source_socket;

typedef struct janus_source_session {
	janus_plugin_session *handle;
	gboolean has_audio;
	gboolean has_video;
	gboolean audio_active;
	gboolean video_active;
	uint64_t bitrate;
	janus_recorder *arc;	/* The Janus recorder instance for this user's audio, if enabled */
	janus_recorder *vrc;	/* The Janus recorder instance for this user's video, if enabled */
	janus_mutex rec_mutex;	/* Mutex to protect the recorders from race conditions */
	guint16 slowlink_count;
	volatile gint hangingup;
	gint64 destroyed;	/* Time at which this session was marked as destroyed */
	GMainLoop *loop;
	GThread *rtsp_thread;
	janus_source_socket rtp_video;
	janus_source_socket rtcp_video;
	janus_source_socket rtp_audio;
	janus_source_socket rtcp_audio;
	gchar * db_entry_session_id;
	gint periodic_pli;
} janus_source_session;
static GHashTable *sessions;
static GList *old_sessions;
static janus_mutex sessions_mutex;
static GHashTable *sessions;
static janus_mutex ports_pool_mutex;
static ports_pool * pp;
static CURL *curl_handle = NULL;

/* configuration options */
static uint16_t udp_min_port = 0, udp_max_port = 0;
static gchar *status_service_url = NULL;
//function declarations
static void *janus_source_rtsp_server_thread(void *data);
static void client_connected_cb(GstRTSPServer *gstrtspserver, GstRTSPClient *gstrtspclient, gpointer data);
static void janus_source_request_keyframe(janus_source_session *session);
static gboolean request_key_frame_cb(gpointer data);
static gboolean request_key_frame_periodic_cb(gpointer data);
static void media_configure_cb(GstRTSPMediaFactory * factory, GstRTSPMedia * media, gpointer data);
static void client_play_request_cb(GstRTSPClient  *gstrtspclient, GstRTSPContext *rtspcontext, gpointer data);
static void janus_source_close_session_func(gpointer key, gpointer value, gpointer user_data);
static void janus_source_close_session(janus_source_session * session);
static void janus_source_relay_rtp(janus_source_session *session, int video, char *buf, int len);
static void janus_source_relay_rtcp(janus_source_session *session, int video, char *buf, int len);
static gboolean janus_source_create_socket(janus_source_socket * sck);
static void janus_source_close_socket(janus_source_socket * sck);
static void janus_source_parse_ports_range(janus_config_item *ports_range, uint16_t * udp_min_port, uint16_t * udp_max_port);
static void rtsp_media_new_state_cb(GstRTSPMedia *gstrtspmedia, gint arg1, gpointer user_data);
static void media_configure_cb(GstRTSPMediaFactory * factory, GstRTSPMedia * media, gpointer data);
static GstRTSPFilterResult janus_source_close_rtsp_sessions(GstRTSPSessionPool *pool, GstRTSPSession *session, gpointer data);
static void janus_source_parse_status_service_url(janus_config_item *config_url, gchar **url);

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
			cl = cl->next;
		}
		janus_config_destroy(config);
		config = NULL;
	}

	if (udp_min_port <= 0 || udp_max_port <= 0) {
		udp_min_port = 4000;
		udp_max_port = 5000;
		JANUS_LOG(LOG_ERR, "Using default port range: %d-%d\n", udp_min_port, udp_max_port);
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
	curl_handle = curl_init();
	
	janus_mutex_init(&ports_pool_mutex);
	//todo: read from config file
	ports_pool_init(&pp, udp_min_port, udp_max_port);
	
	/* Launch the thread that will handle incoming messages */
	handler_thread = g_thread_try_new("janus source handler", janus_source_handler, NULL, &error);
	if (error != NULL) {
		g_atomic_int_set(&initialized, 0);
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the Source handler thread...\n", error->code, error->message ? error->message : "??");
		return -1;
	}

	curl_handle = curl_init();

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
	if (watchdog != NULL) {
		g_thread_join(watchdog);
		watchdog = NULL;
	}

	g_hash_table_foreach(sessions, janus_source_close_session_func, NULL);
	janus_mutex_lock(&ports_pool_mutex);
	ports_pool_free(pp);
	janus_mutex_unlock(&ports_pool_mutex);
	
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
	session->has_audio = FALSE;
	session->has_video = FALSE;
	session->audio_active = TRUE;
	session->video_active = TRUE;


	memset(&session->rtp_video, 0, sizeof(session->rtp_video));
	memset(&session->rtcp_video, 0, sizeof(session->rtcp_video));
	memset(&session->rtp_audio, 0, sizeof(session->rtp_audio));
	memset(&session->rtcp_audio, 0, sizeof(session->rtcp_audio));

	session->loop = NULL;
	session->rtsp_thread = NULL;
	session->periodic_pli = 0;

	janus_mutex_init(&session->rec_mutex);
	session->bitrate = 0;	/* No limit */
	session->destroyed = 0;
	g_atomic_int_set(&session->hangingup, 0);
	handle->plugin_handle = session;

	if (!janus_source_create_socket(&session->rtp_video)) {
		JANUS_LOG(LOG_FATAL, "Unable to create video RTP sockets\n");
		*error = -3;
	}

	if (!janus_source_create_socket(&session->rtcp_video)) {
		JANUS_LOG(LOG_FATAL, "Unable to create video RTCP sockets\n");
		*error = -4;
	}

	if (!janus_source_create_socket(&session->rtp_audio)) {
		JANUS_LOG(LOG_FATAL, "Unable to create audio RTP sockets\n");
		*error = -5;
	}

	if (!janus_source_create_socket(&session->rtcp_audio)) {
		JANUS_LOG(LOG_FATAL, "Unable to create audio RTCP sockets\n");
		*error = -6;
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

    
	gboolean retCode =
	curl_request(curl_handle,g_strdup_printf("%s/%s",status_service_url,g_strdup(session->db_entry_session_id)),"{}","DELETE",NULL,FALSE);
        if(retCode != TRUE){
            JANUS_LOG(LOG_ERR,"Could not send the request to the server\n"); 
        }


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
	
	JANUS_LOG(LOG_ERR, "janus_source_setup_media: video_active: %d, audio_active: %d, has_video: %d, has_audio: %d\n", session->video_active, session->audio_active, session->has_video, session->has_audio);

	session->rtsp_thread = g_thread_try_new("rtsp server", janus_source_rtsp_server_thread, session, NULL); 
	if (!session->rtsp_thread) {
		JANUS_LOG(LOG_ERR, "RTSP thread creation failure\n");
	}

	session->periodic_pli = g_timeout_add(5000, request_key_frame_periodic_cb, (gpointer)session);
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
		if (session->bitrate > 0)
			janus_rtcp_cap_remb(buf, len, session->bitrate);
		//gateway->relay_rtcp(handle, video, buf, len);
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
	session->has_audio = FALSE;
	session->has_video = FALSE;
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
		/* Any SDP to handle? */
		if (msg->sdp) {
			JANUS_LOG(LOG_VERB, "This is involving a negotiation (%s) as well:\n%s\n", msg->sdp_type, msg->sdp);
			session->has_audio = (strstr(msg->sdp, "m=audio") != NULL);
			session->has_video = (strstr(msg->sdp, "m=video") != NULL);
		}

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
	GSocket * sock_rtp_cli = video ? session->rtp_video.socket : session->rtp_audio.socket;

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

	GSocket * sock_rtcp_cli = video ? session->rtcp_video.socket : session->rtcp_audio.socket;

	if (!sock_rtcp_cli)
		return;

	if (g_socket_send(sock_rtcp_cli, buf, len, NULL, NULL) < 0) {
		//JANUS_LOG(LOG_ERR, "Send RTCP failed! type: %s\n", video ? "video" : "audio");
	}
	else {
		//JANUS_LOG(LOG_ERR, "Send RTCP successfully! type: %s; len=%d\n", video ? "video" : "audio", len);
	}
}


static gboolean janus_source_create_socket(janus_source_socket * sck) {

	GSocketAddress * address = NULL;
	gboolean result = FALSE;
	int port;

	sck->socket = g_socket_new(G_SOCKET_FAMILY_IPV4,
		G_SOCKET_TYPE_DATAGRAM,
		G_SOCKET_PROTOCOL_UDP,
		NULL);

	if (!sck->socket) {
		JANUS_LOG(LOG_ERR, "Error creating socket\n");
		return FALSE;
	}
	
	do
	{
		janus_mutex_lock(&ports_pool_mutex);
		port = ports_pool_get(pp, 0);
		janus_mutex_unlock(&ports_pool_mutex);
		
		if (!port) {
			JANUS_LOG(LOG_ERR, "No free ports available in ports pool\n");
			break;
		}
		
		address = g_inet_socket_address_new_from_string("127.0.0.1", port);

		if (!address) {
			JANUS_LOG(LOG_ERR, "Error while creating address\n");
			break;
		}
	
		result = g_socket_connect(sck->socket, address, NULL, NULL);

		if (!result) {
			JANUS_LOG(LOG_ERR, "Connect failed on port: %d\n", port);
		}
		
		if (!result) {
			janus_mutex_lock(&ports_pool_mutex);
			ports_pool_return(pp, port);
			janus_mutex_unlock(&ports_pool_mutex);
		}
	} while (!result);


	if (!result) {
		janus_source_close_socket(sck);
	}
	else {
		sck->port = port;
		g_assert(port);
	}

	g_clear_object(&address);
	return result;
}

static void janus_source_close_socket(janus_source_socket * sck) {
	if (sck->socket) {
		g_socket_close(sck->socket, NULL);
		g_clear_object(&sck->socket);
	}
	
	janus_mutex_lock(&ports_pool_mutex);
	ports_pool_return(pp, sck->port);
	janus_mutex_unlock(&ports_pool_mutex);
}


static void rtsp_media_new_state_cb(GstRTSPMedia *gstrtspmedia, gint arg1, gpointer user_data)
{ 
	//todo: remove
	JANUS_LOG(LOG_INFO, "rtsp_media_new_state_cb: %d\n", (GstState)arg1);
}

static void media_configure_cb(GstRTSPMediaFactory * factory, GstRTSPMedia * media, gpointer data)
{
	JANUS_LOG(LOG_INFO, "media_configure callback\n") ;
	
	g_signal_connect(media, "new-state", (GCallback)rtsp_media_new_state_cb, data);
#if 0
	GstElement * pipeline ;
	GstElement *src ;
	gint port ;

	pipeline = gst_rtsp_media_get_element(media) ;

	src = gst_bin_get_by_name(GST_BIN(pipeline), "udp_rtp_src_video") ;
	g_object_get(src, "port", &port, NULL) ;
	g_print("Read udp port: %d\n", port) ;
#endif
}

static void
client_play_request_cb(GstRTSPClient  *gstrtspclient,
	GstRTSPContext *rtspcontext,
	gpointer        data)
{
	JANUS_LOG(LOG_INFO, "client_play_request_cb\n") ;
	//todo: find better callback for triggering this
	g_timeout_add(1000, request_key_frame_cb, data) ;
}

static void
client_connected_cb(GstRTSPServer *gstrtspserver,
	GstRTSPClient *gstrtspclient,
	gpointer       data)
{
	JANUS_LOG(LOG_INFO, "New client connected\n");		
	
	g_signal_connect(gstrtspclient, "play-request", (GCallback)client_play_request_cb, data);
	g_timeout_add(1000, request_key_frame_cb, data);

}

static GstRTSPFilterResult
janus_source_close_rtsp_sessions(GstRTSPSessionPool *pool, GstRTSPSession *session, gpointer data) { 
	JANUS_LOG(LOG_INFO, "Removing RTSP session: %s\n", gst_rtsp_session_get_sessionid(session));
	return GST_RTSP_FILTER_REMOVE;	
}

static void *janus_source_rtsp_server_thread(void *data) {

	GMainLoop *loop;
	GstRTSPServer *server;
	GstRTSPMountPoints *mounts;
	GstRTSPMediaFactory *factory;
	GstRTSPSessionPool *session_pool;
	GList * sessions_list;
	gchar * launch_pipe = NULL;
	int rtsp_port;

	const gchar *http_post = g_strdup("POST");
	janus_source_session *session = (janus_source_session *)data;
    


	if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		JANUS_LOG(LOG_INFO, "Plugin is stopping\n");
		return FALSE;
	}

	if (!session) {
		JANUS_LOG(LOG_ERR, "Session is NULL\n");
		return FALSE;
	}

	if (g_atomic_int_get(&session->hangingup) || session->destroyed) {
		JANUS_LOG(LOG_INFO, "Session is being destroyed\n");
		return FALSE;
	}

	int port_rtp_video = session->rtp_video.port;
	int port_rtp_audio = session->rtp_audio.port;

	JANUS_LOG(LOG_INFO, "udpsrc RTP video port: %d\n", port_rtp_video);
	JANUS_LOG(LOG_INFO, "udpsrc RTP audio port: %d\n", port_rtp_audio);

	server = gst_rtsp_server_new();

	gst_rtsp_server_set_address(server, janus_get_local_ip());
	
	/* Allocate random port */
	gst_rtsp_server_set_service(server, "0");

	/* make a mainloop for the default context */
	loop = g_main_loop_new(NULL, FALSE);
	session->loop = loop;

	/* attach the server to the default maincontext */
	if (gst_rtsp_server_attach(server, NULL) == 0) {
		JANUS_LOG(LOG_ERR, "failed to attach the server\n");
		goto error;
	}

	factory = gst_rtsp_media_factory_new();

	gst_rtsp_media_factory_set_latency(factory, 0);
	
	/* todo: use SDP to dynamically recognize content type */
	if (session->has_video && session->has_audio)
	{
		launch_pipe = g_strdup_printf(
			"( udpsrc port=%d name=udp_rtp_src_video caps=\"application/x-rtp, media=video, encoding-name=VP8\"  ! rtpvp8depay  ! rtpvp8pay pt=96 name=pay0 "
			"  udpsrc port=%d name=udp_rtp_src_audio caps=\"application/x-rtp, media=audio, encoding-name=OPUS\" ! rtpopusdepay ! audio/x-opus, channels=1 ! rtpopuspay pt=127 name=pay1 )",
			port_rtp_video,
			port_rtp_audio);
	}
	else if (session->has_video && !session->has_audio)
	{
		launch_pipe = g_strdup_printf("( udpsrc port=%d name=udp_rtp_src_video caps=\"application/x-rtp, media=video, encoding-name=VP8\"  ! rtpvp8depay  ! rtpvp8pay pt=96 name=pay0 )",
			port_rtp_video);
	}
	else if (!session->has_video && session->has_audio)
	{
		launch_pipe = g_strdup_printf(
			"( udpsrc port=%d name=udp_rtp_src_audio caps=\"application/x-rtp, media=audio, encoding-name=OPUS\" ! rtpopusdepay ! audio/x-opus, channels=1 ! rtpopuspay pt=127 name=pay0 )",
			port_rtp_audio);
	}
	
	gst_rtsp_media_factory_set_launch(factory, launch_pipe);
	g_free(launch_pipe);

	/* media created from this factory can be shared between clients */
	gst_rtsp_media_factory_set_shared(factory, TRUE);


#if 0
	GstRTSPUrl *url = NULL;
	GstElement * bin = NULL;
	GstRTSPResult res;

	//reduce buffer size of UDPSRC
	res = gst_rtsp_url_parse("rtsp://localhost/camera", &url);
	g_assert(res == GST_RTSP_OK);

	bin = gst_rtsp_media_factory_create_element(factory, url);
	g_assert(bin);

	GstElement * src_video = gst_bin_get_by_name(GST_BIN(bin), "udp_rtp_src_video");
	g_assert(src_video);
	g_object_set(src_video, "buffer-size", 2048, NULL);

	GstElement * src_audio = gst_bin_get_by_name(GST_BIN(bin), "udp_rtp_src_audio");
	g_assert(src_audio);
	g_object_set(src_audio, "buffer-size", 2048, NULL);

#endif

	g_signal_connect(factory, "media-configure", (GCallback)media_configure_cb,
		(gpointer)session);

	g_signal_connect(server, "client-connected", (GCallback)client_connected_cb,
		(gpointer)session);

	/* get the default mount points from the server */
	mounts = gst_rtsp_server_get_mount_points(server);

	/* attach the session to the "/camera" URL */
	gst_rtsp_mount_points_add_factory(mounts, "/camera", factory);
	g_object_unref(mounts);
	
	rtsp_port = gst_rtsp_server_get_bound_port(server);

	gchar *rtsp_url = g_strdup_printf("rtsp://%s:%d/camera", janus_get_public_ip(), rtsp_port);
	
	gboolean retCode = curl_request(curl_handle, status_service_url,rtsp_url,http_post,&(session->db_entry_session_id),TRUE);
	if(retCode != TRUE){
	    JANUS_LOG(LOG_ERR,"Could not send the request to the server\n"); 
	}
	
	JANUS_LOG(LOG_INFO, "Stream ready at %s\n", rtsp_url);
	g_free(rtsp_url);

	g_main_loop_run(loop);
	
	session_pool = gst_rtsp_server_get_session_pool(server);
	sessions_list = gst_rtsp_session_pool_filter(session_pool, janus_source_close_rtsp_sessions, NULL);
	g_list_free_full(sessions_list, gst_object_unref);
	g_object_unref(session_pool);

error:
	JANUS_LOG(LOG_INFO, "Freeing RTSP server\n");
	g_object_unref(server);
	g_main_loop_unref(loop);
	session->loop = NULL;
	return NULL;
}

static void janus_source_request_keyframe(janus_source_session *session)
{
	if (!session) {
		JANUS_LOG(LOG_ERR, "keyframe_once_cb: session is NULL\n");
		return;
	}

	if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized) || g_atomic_int_get(&session->hangingup) || session->destroyed) {
		JANUS_LOG(LOG_INFO, "Keyframe generation event while plugin or session is stopping\n");
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

static void janus_source_close_session_func(gpointer key, gpointer value, gpointer user_data) {

	if (value != NULL) {
		janus_source_close_session((janus_source_session *)value);
	}
}

static void janus_source_close_session(janus_source_session * session) {
	JANUS_LOG(LOG_INFO, "Closing source session\n");

	janus_source_close_socket(&session->rtp_video);
	janus_source_close_socket(&session->rtcp_video);
	janus_source_close_socket(&session->rtp_audio);
	janus_source_close_socket(&session->rtcp_audio);

	if (session->periodic_pli) {
		g_source_remove(session->periodic_pli);
	}

	if (session->loop) {
		g_main_loop_quit(session->loop);
	}

	if (session->rtsp_thread != NULL) {
		g_thread_join(session->rtsp_thread);
		session->rtsp_thread = NULL;
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

