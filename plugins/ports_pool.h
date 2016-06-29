#include <glib.h>

typedef gint64 port_t;

typedef struct ports_pool
{
	port_t   min;
	port_t   max;
	GList*   list;
	gint     count;
} ports_pool;



void ports_pool_init(ports_pool ** pp, port_t min, port_t max);
void ports_pool_free(ports_pool * pp);
gint ports_pool_get(ports_pool * pp, port_t port);
void ports_pool_return(ports_pool * pp, port_t port);
