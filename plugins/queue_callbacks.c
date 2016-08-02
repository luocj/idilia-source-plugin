#include "queue_callbacks.h"

extern GAsyncQueue *rtsp_async_queue;

GSource *
queue_source_new (GAsyncQueue *queue){
    GSource *source; 
    MessageQueueSource *message_queue_source;
    
    source = g_source_new (&source_func, sizeof (MessageQueueSource));

    message_queue_source = (MessageQueueSource *) source;

    message_queue_source->queue = g_async_queue_ref (queue);
    return source;
}

gboolean
queue_events_callback(gpointer data)
{
    if(NULL != data) {
	    QueueEventData *queue_data = (QueueEventData*)data;
	    queue_data->callback(queue_data->session);
    }

	return TRUE;
}

gboolean
queue_events_dispatch(GSource *source, GSourceFunc callback, gpointer user_data)
{
    MessageQueueSource *queue = (MessageQueueSource *) source;
	gboolean result = FALSE;
	gpointer data = g_async_queue_try_pop(queue->queue);
	if (data != NULL) {
		result = callback(data);
	}
	
	g_free(data);
	return result;
}
  
gboolean 
queue_prepare(GSource *source, gint *timeout)
{
    MessageQueueSource *queue = (MessageQueueSource *) source;
	return g_async_queue_length(queue->queue) > 0;
}

