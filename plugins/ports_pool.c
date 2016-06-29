#include <glib.h>
#include "ports_pool.h"

void ports_pool_init(ports_pool ** pp, port_t min, port_t max)
{
	*pp = g_malloc(sizeof(ports_pool));
	(**pp).list = NULL;
	(**pp).min = min;
	(**pp).max = max;
	(**pp).count = 0;
	
}

void ports_pool_free(ports_pool * pp)
{
	g_list_free(pp->list);
	g_free(pp);
	pp = NULL;
}

gint ports_pool_get(ports_pool * pp, port_t port)
{
	if (pp->count >= (pp->max - pp->min))
	{
		//no free ports
		return 0;
	}

	if (port >= pp->min && port <= pp->max) {
		if (g_list_find(pp->list, (gconstpointer)port)) {
			port = 0;
		}
	}
	else
	{
		do {
			
			port = g_random_int_range(pp->min, pp->max);
			
		} while (g_list_find(pp->list, (gconstpointer)port));
	}
	
	if (port > 0) {
		pp->list = g_list_append(pp->list, (gpointer)port);
		pp->count++;
	}

	return port;
} 

void ports_pool_return(ports_pool * pp, port_t port)
{
	pp->list = g_list_remove(pp->list, (gconstpointer)port);
	pp->count--;
}
