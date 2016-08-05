#pragma once

#include <gio/gio.h>
#include <stdint.h>

typedef struct janus_source_socket {
	int port;
	GSocket *socket;
	gboolean is_client;
	GSource *source;
} janus_source_socket;

void socket_utils_init(uint16_t udp_min_port, uint16_t udp_max_port);
void socket_utils_destroy(void);
gboolean socket_utils_create_client_socket(janus_source_socket * sck, int port_to_connect);
gboolean socket_utils_create_server_socket(janus_source_socket * sck);
void socket_utils_close_socket(janus_source_socket * sck);
void socket_utils_attach_callback(janus_source_socket * sck, GSourceFunc func, gpointer * user_data);
void socket_utils_deattach_callback(janus_source_socket * sck);
