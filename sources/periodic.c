
/*
 * Odissey.
 *
 * Advanced PostgreSQL connection pooler.
*/

#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <signal.h>

#include <machinarium.h>
#include <shapito.h>

#include "sources/macro.h"
#include "sources/version.h"
#include "sources/list.h"
#include "sources/pid.h"
#include "sources/id.h"
#include "sources/syslog.h"
#include "sources/log.h"
#include "sources/daemon.h"
#include "sources/scheme.h"
#include "sources/lex.h"
#include "sources/config.h"
#include "sources/msg.h"
#include "sources/system.h"
#include "sources/instance.h"
#include "sources/server.h"
#include "sources/server_pool.h"
#include "sources/client.h"
#include "sources/client_pool.h"
#include "sources/route_id.h"
#include "sources/route.h"
#include "sources/route_pool.h"
#include "sources/io.h"
#include "sources/router.h"
#include "sources/pooler.h"
#include "sources/relay.h"
#include "sources/frontend.h"
#include "sources/backend.h"
#include "sources/periodic.h"

static inline void
od_periodic_stats(od_router_t *router)
{
	od_instance_t *instance = router->system->instance;
	if (router->route_pool.count == 0)
		return;
	od_log(&instance->log, "statistics");
	od_list_t *i;
	od_list_foreach(&router->route_pool.list, i) {
		od_route_t *route;
		route = od_container_of(i, od_route_t, link);
		od_log(&instance->log,
		       "  [%.*s, %.*s] clients %d, "
		       "pool_active %d, "
		       "pool_idle %d ",
		       route->id.database_len,
		       route->id.database,
		       route->id.user_len,
		       route->id.user,
			   od_clientpool_total(&route->client_pool),
		       route->server_pool.count_active,
		       route->server_pool.count_idle);
	}
}

static inline int
od_expire_mark(od_server_t *server, void *arg)
{
	od_router_t *router = arg;
	od_instance_t *instance = router->system->instance;

	od_route_t *route = server->route;
	/*
	if (! machine_connected(server->io)) {
		od_serverpool_set(&route->server_pool, server,
		                  OD_SCLOSE);
		return 0;
	}
	*/
	if (! route->scheme->pool_ttl)
		return 0;
	od_debug_server(&instance->log, &server->id, "expire",
	                "idle time: %d",
	                server->idle_time);
	if (server->idle_time < route->scheme->pool_ttl) {
		server->idle_time++;
		return 0;
	}
	od_serverpool_set(&route->server_pool, server,
	                  OD_SEXPIRE);
	return 0;
}

static void
od_periodic(void *arg)
{
	od_periodic_t *periodic = arg;
	od_router_t *router = periodic->system->router;
	od_instance_t *instance = periodic->system->instance;

	int tick = 0;
	for (;;)
	{
		/* idle servers expire.
		 *
		 * 1. Add plus one idle second on each traversal.
		 *    If a server idle time is equal to ttl, then move
		 *    it to the EXPIRE queue.
		 *
		 *    It is important that this function must not yield.
		 *
		 * 2. Foreach servers in EXPIRE queue, send Terminate
		 *    and close the connection.
		*/

		/* mark servers for gc */
		od_routepool_foreach(&router->route_pool, OD_SIDLE,
		                     od_expire_mark,
		                     router);

#if 0
		/* sweep disconnected servers */
		for (;;) {
			od_server_t *server;
			server = od_routepool_next(&router->route_pool, OD_SCLOSE);
			if (server == NULL)
				break;
			od_log(&instance->log, server->io, "S: disconnected",
			       server->idle_time);
			server->idle_time = 0;
			/*
			od_beclose(server);
			*/
		}
#endif

		/* sweep expired connections */
		for (;;) {
			od_server_t *server;
			server = od_routepool_next(&router->route_pool, OD_SEXPIRE);
			if (server == NULL)
				break;
			od_debug_server(&instance->log, &server->id, "expire",
			                "closing idle connection (%d secs)",
			                server->idle_time);
			server->idle_time = 0;

			od_route_t *route = server->route;
			server->route = NULL;
			od_serverpool_set(&route->server_pool, server, OD_SUNDEF);

			machine_io_attach(server->io);

			od_backend_terminate(server);
			od_backend_close(server);

			/* cleanup unused dynamic routes */
			od_routepool_gc(&router->route_pool);
		}

		/* stats */
		if (instance->scheme.log_statistics > 0) {
			tick++;
			if (tick >= instance->scheme.log_statistics) {
				od_periodic_stats(router);
				tick = 0;
			}
		}

		/* 1 second soft interval */
		machine_sleep(1000);
	}
}

int od_periodic_init(od_periodic_t *periodic, od_system_t *system)
{
	periodic->system = system;
	return 0;
}

int od_periodic_start(od_periodic_t *periodic)
{
	od_instance_t *instance = periodic->system->instance;
	int64_t coroutine_id;
	coroutine_id = machine_coroutine_create(od_periodic, periodic);
	if (coroutine_id == -1) {
		od_error(&instance->log, "periodic", "failed to start periodic coroutine");
		return -1;
	}
	return 0;
}
