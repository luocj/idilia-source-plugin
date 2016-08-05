#include "socket_utils.h"
#include "ports_pool.h"
#include "debug.h"
#include "mutex.h"


static janus_mutex ports_pool_mutex;
static ports_pool * pp;

static gboolean socket_utils_create_socket(janus_source_socket * sck, gboolean is_client, int req_port);

void socket_utils_init(uint16_t udp_min_port, uint16_t udp_max_port)
{
	janus_mutex_init(&ports_pool_mutex);
	ports_pool_init(&pp, udp_min_port, udp_max_port);
}

void socket_utils_destroy(void)
{
	janus_mutex_lock(&ports_pool_mutex);
	ports_pool_free(pp);
	janus_mutex_unlock(&ports_pool_mutex);
}

gboolean socket_utils_create_client_socket(janus_source_socket * sck, int port_to_connect) {
	return socket_utils_create_socket(sck, TRUE, port_to_connect);
}

gboolean socket_utils_create_server_socket(janus_source_socket * sck) {
	return socket_utils_create_socket(sck, FALSE, 0);
}

static gboolean socket_utils_create_socket(janus_source_socket * sck, gboolean is_client, int req_port) {

	GSocketAddress * address = NULL;
	gboolean result = TRUE;
	GError *error = NULL;
	int port;
	
	sck->source = NULL;

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
		if (!req_port) {
			janus_mutex_lock(&ports_pool_mutex);
			port = ports_pool_get(pp, 0);
			janus_mutex_unlock(&ports_pool_mutex);
		}
		else {
			port = req_port;
		}
		
		if (!port) {
			JANUS_LOG(LOG_ERR, "No free ports available in ports pool\n");
			break;
		}
		
		address = g_inet_socket_address_new_from_string("127.0.0.1", port);

		if (!address) {
			JANUS_LOG(LOG_ERR, "Error while creating address\n");
			break;
		}

		sck->is_client = is_client;

		if (is_client) {
			result = g_socket_connect(sck->socket, address, NULL, &error);
			if (!result) {
				JANUS_LOG(LOG_ERR, "Connect failed on port: %d; error: %s\n", port, error->message);
				g_error_free(error);
			}

		}
		else {
			result = g_socket_bind(sck->socket, address, TRUE, &error);
			if (!result) {
				JANUS_LOG(LOG_ERR, "Error while binding udp socket: %s\n", error->message);
				g_error_free(error);
			}
		}

		if (!result) {
			janus_mutex_lock(&ports_pool_mutex);
			ports_pool_return(pp, port);
			janus_mutex_unlock(&ports_pool_mutex);
		}
	} while (!result);


	if (!result) {
		socket_utils_close_socket(sck);
	}
	else {
		sck->port = port;
		g_assert(port);
	}

	g_clear_object(&address);
	return result;
}


void socket_utils_close_socket(janus_source_socket * sck) {
	
	if (sck->source) {
		socket_utils_deattach_callback(sck);
	}
	
	if (sck->socket) {
		g_socket_close(sck->socket, NULL);
		g_clear_object(&sck->socket);
	}
	
	janus_mutex_lock(&ports_pool_mutex);
	ports_pool_return(pp, sck->port);
	janus_mutex_unlock(&ports_pool_mutex);
}

void socket_utils_attach_callback(janus_source_socket * sck, GSourceFunc func, gpointer * user_data) {
	sck->source = g_socket_create_source(sck->socket, G_IO_IN, NULL);
	g_assert(sck->source);
	g_source_set_callback(sck->source, func, user_data, NULL);
	g_source_attach(sck->source, g_main_context_default());
}

void socket_utils_deattach_callback(janus_source_socket * sck) {
	g_source_destroy(sck->source);
	g_source_unref(sck->source);
	sck->source = NULL;
}
