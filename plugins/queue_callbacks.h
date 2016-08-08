#pragma once
#include <glib.h>
#include <gst/rtsp-server/rtsp-server.h>

typedef void(*QueueEventCallback)(gpointer session);

typedef struct QueueEventData {
	QueueEventCallback callback;
	gpointer session;
} QueueEventData;

GSource *
queue_source_new (GAsyncQueue  *queue);
gboolean
queue_events_callback(gpointer data);
gboolean
queue_events_dispatch(GSource *source, GSourceFunc callback, gpointer user_data);
gboolean queue_prepare(GSource *source, gint *timeout);


typedef struct {
  GSource         parent;
  GAsyncQueue    *queue;
} MessageQueueSource;
