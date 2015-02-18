/*
 * This file is distributed as part of MaxScale.  It is free
 * software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation,
 * version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright MariaDB Corporation Ab 2014
 */

/**
 * @file maxinfo.c - A "routing module" that in fact merely gives access
 * to a MaxScale information schema usign the MySQL protocol
 *
 * @verbatim
 * Revision History
 *
 * Date		Who		Description
 * 16/02/15	Mark Riddoch	Initial implementation
 *
 * @endverbatim
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <service.h>
#include <session.h>
#include <router.h>
#include <modules.h>
#include <modinfo.h>
#include <modutil.h>
#include <atomic.h>
#include <spinlock.h>
#include <dcb.h>
#include <poll.h>
#include <maxinfo.h>
#include <skygw_utils.h>
#include <log_manager.h>
#include <resultset.h>
#include <version.h>


MODULE_INFO 	info = {
	MODULE_API_ROUTER,
	MODULE_GA,
	ROUTER_VERSION,
	"The MaxScale Information Schema"
};

/** Defined in log_manager.cc */
extern int            lm_enabled_logfiles_bitmask;
extern size_t         log_ses_count[];
extern __thread log_info_t tls_log_info;

static char *version_str = "V1.0.0";

static int maxinfo_statistics(INFO_INSTANCE *, INFO_SESSION *, GWBUF *);
static int maxinfo_ping(INFO_INSTANCE *, INFO_SESSION *, GWBUF *);
static int maxinfo_execute_query(INFO_INSTANCE *, INFO_SESSION *, char *);


/* The router entry points */
static	ROUTER	*createInstance(SERVICE *service, char **options);
static	void	*newSession(ROUTER *instance, SESSION *session);
static	void 	closeSession(ROUTER *instance, void *router_session);
static	void 	freeSession(ROUTER *instance, void *router_session);
static	int	execute(ROUTER *instance, void *router_session, GWBUF *queue);
static	void	diagnostics(ROUTER *instance, DCB *dcb);
static  uint8_t getCapabilities (ROUTER* inst, void* router_session);
static  void             handleError(
        ROUTER           *instance,
        void             *router_session,
        GWBUF            *errbuf,
        DCB              *backend_dcb,
        error_action_t   action,
        bool             *succp);

/** The module object definition */
static ROUTER_OBJECT MyObject = {
    createInstance,
    newSession,
    closeSession,
    freeSession,
    execute,
    diagnostics,
    NULL,
    handleError,
    getCapabilities
};

static SPINLOCK		instlock;
static INFO_INSTANCE	*instances;

/**
 * Implementation of the mandatory version entry point
 *
 * @return version string of the module
 */
char *
version()
{
	return version_str;
}

/**
 * The module initialisation routine, called when the module
 * is first loaded.
 */
void
ModuleInit()
{
	LOGIF(LM, (skygw_log_write(
                           LOGFILE_MESSAGE,
                           "Initialise MaxInfo router module %s.\n",
                           version_str)));
	spinlock_init(&instlock);
	instances = NULL;
}

/**
 * The module entry point routine. It is this routine that
 * must populate the structure that is referred to as the
 * "module object", this is a structure with the set of
 * external entry points for this module.
 *
 * @return The module object
 */
ROUTER_OBJECT *
GetModuleObject()
{
	return &MyObject;
}

/**
 * Create an instance of the router for a particular service
 * within the gateway.
 * 
 * @param service	The service this router is being create for
 * @param options	Any array of options for the query router
 *
 * @return The instance data for this new instance
 */
static	ROUTER	*
createInstance(SERVICE *service, char **options)
{
INFO_INSTANCE	*inst;
int		i;

	if ((inst = malloc(sizeof(INFO_INSTANCE))) == NULL)
		return NULL;

	inst->service = service;
	spinlock_init(&inst->lock);

	if (options)
	{
		for (i = 0; options[i]; i++)
		{
			{
				LOGIF(LE, (skygw_log_write(
					LOGFILE_ERROR,
					"Unknown option for MaxInfo '%s'\n",
					options[i])));
			}
		}
	}

	/*
	 * We have completed the creation of the instance data, so now
	 * insert this router instance into the linked list of routers
	 * that have been created with this module.
	 */
	spinlock_acquire(&instlock);
	inst->next = instances;
	instances = inst;
	spinlock_release(&instlock);

	service->users = mysql_users_alloc();
	add_mysql_users_with_host_ipv4(service->users, "massi", "%", "2CFEB4BD447B9BC5D591249377EF5A7E340D1A1D", "Y", "");
	add_mysql_users_with_host_ipv4(service->users, "massi", "localhost", "2CFEB4BD447B9BC5D591249377EF5A7E340D1A1D", "Y", "");
	add_mysql_users_with_host_ipv4(service->users, "monitor", "%", "", "Y", "");
	add_mysql_users_with_host_ipv4(service->users, "monitor", "localhost", "", "Y", "");

	return (ROUTER *)inst;
}

/**
 * Associate a new session with this instance of the router.
 *
 * @param instance	The router instance data
 * @param session	The session itself
 * @return Session specific data for this session
 */
static	void	*
newSession(ROUTER *instance, SESSION *session)
{
INFO_INSTANCE	*inst = (INFO_INSTANCE *)instance;
INFO_SESSION	*client;

	if ((client = (INFO_SESSION *)malloc(sizeof(INFO_SESSION))) == NULL)
	{
		return NULL;
	}
	client->session = session;
	client->dcb = session->client;
	client->queue = NULL;

	spinlock_acquire(&inst->lock);
	client->next = inst->sessions;
	inst->sessions = client;
	spinlock_release(&inst->lock);

	session->state = SESSION_STATE_READY;

	return (void *)client;
}

/**
 * Close a session with the router, this is the mechanism
 * by which a router may cleanup data structure etc.
 *
 * @param instance		The router instance data
 * @param router_session	The session being closed
 */
static	void 	
closeSession(ROUTER *instance, void *router_session)
{
INFO_INSTANCE	*inst = (INFO_INSTANCE *)instance;
INFO_SESSION	*session = (INFO_SESSION *)router_session;


	spinlock_acquire(&inst->lock);
	if (inst->sessions == session)
		inst->sessions = session->next;
	else
	{
		INFO_SESSION *ptr = inst->sessions;
		while (ptr && ptr->next != session)
			ptr = ptr->next;
		if (ptr)
			ptr->next = session->next;
	}
	spinlock_release(&inst->lock);
        /**
         * Router session is freed in session.c:session_close, when session who
         * owns it, is freed.
         */
}

/**
 * Free a maxinfo session
 *
 * @param router_instance	The router session
 * @param router_client_session	The router session as returned from newSession
 */
static void freeSession(
        ROUTER* router_instance,
        void*   router_client_session)
{
	free(router_client_session);
        return;
}

/**
 * Error Handler routine
 *
 * The routine will handle errors that occurred in backend writes.
 *
 * @param       instance        The router instance
 * @param       router_session  The router session
 * @param       message         The error message to reply
 * @param       backend_dcb     The backend DCB
 * @param       action     	The action: REPLY, REPLY_AND_CLOSE, NEW_CONNECTION
 *
 */
static void handleError(
	ROUTER           *instance,
	void             *router_session,
	GWBUF            *errbuf,
	DCB              *backend_dcb,
	error_action_t   action,
	bool             *succp)

{
	DCB             *client_dcb;
	SESSION         *session = backend_dcb->session;
	session_state_t sesstate;

	/** Reset error handle flag from a given DCB */
	if (action == ERRACT_RESET)
	{
		backend_dcb->dcb_errhandle_called = false;
		return;
	}
	
	/** Don't handle same error twice on same DCB */
	if (backend_dcb->dcb_errhandle_called)
	{
		/** we optimistically assume that previous call succeed */
		*succp = true;
		return;
	}
	else
	{
		backend_dcb->dcb_errhandle_called = true;
	}
	spinlock_acquire(&session->ses_lock);
	sesstate = session->state;
	client_dcb = session->client;
	
	if (sesstate == SESSION_STATE_ROUTER_READY)
	{
		CHK_DCB(client_dcb);
		spinlock_release(&session->ses_lock);	
		client_dcb->func.write(client_dcb, gwbuf_clone(errbuf));
	}
	else 
	{
		spinlock_release(&session->ses_lock);
	}
	
	/** false because connection is not available anymore */
	*succp = false;
}

/**
 * We have data from the client, this is a SQL command, or other MySQL
 * packet type.
 *
 * @param instance		The router instance
 * @param router_session	The router session returned from the newSession call
 * @param queue			The queue of data buffers to route
 * @return The number of bytes sent
 */
static	int	
execute(ROUTER *rinstance, void *router_session, GWBUF *queue)
{
INFO_INSTANCE	*instance = (INFO_INSTANCE *)rinstance;
INFO_SESSION	*session = (INFO_SESSION *)router_session;
uint8_t		*data;
int		length, len, residual;
char		*sql;

	if (session->queue)
	{
		queue = gwbuf_append(session->queue, queue);
		session->queue = NULL;
		queue = gwbuf_make_contiguous(queue);
	}
	data = (uint8_t *)GWBUF_DATA(queue);
	length = data[0] + (data[1] << 8) + (data[2] << 16);
	if (length + 4 > GWBUF_LENGTH(queue))
	{
		// Incomplete packet, must be buffered
		session->queue = queue;
		return 1;
	}

	// We have a complete request in a signle buffer
	if (modutil_MySQL_Query(queue, &sql, &len, &residual))
	{
		sql = strndup(sql, len);
		return maxinfo_execute_query(instance, session, sql);
	}
	else
	{
		switch (MYSQL_COMMAND(queue))
		{
		case COM_PING:
			return maxinfo_ping(instance, session, queue);
		case COM_STATISTICS:
			return maxinfo_statistics(instance, session, queue);
		default:
			LOGIF(LE, (skygw_log_write_flush(LOGFILE_ERROR,
				"maxinfo: Unexpected MySQL command 0x%x",
				MYSQL_COMMAND(queue))));
		}
	}

	return 1;
}

/**
 * Display router diagnostics
 *
 * @param instance	Instance of the router
 * @param dcb		DCB to send diagnostics to
 */
static	void
diagnostics(ROUTER *instance, DCB *dcb)
{
	return;	/* Nothing to do currently */
}

/**
 * Capabilities interface for the rotuer
 *
 * Not used for the maxinfo router
 */
static uint8_t
getCapabilities(
        ROUTER*  inst,
        void*    router_session)
{
        return 0;
}



/**
 * Return some basic statistics from the router in response to a COM_STATISTICS
 * request.
 *
 * @param router	The router instance
 * @param session	The connection that requested the statistics
 * @param queue		The statistics request
 *
 * @return non-zero on sucessful send
 */
static int
maxinfo_statistics(INFO_INSTANCE *router, INFO_SESSION *session, GWBUF *queue)
{
char	result[1000], *ptr;
GWBUF	*ret;
int	len;
extern	int	MaxScaleUptime();

	snprintf(result, 1000,
		"Uptime: %u  Threads: %u  Sessions: %u ",
			MaxScaleUptime(),
			config_threadcount(),
			serviceSessionCountAll());
	if ((ret = gwbuf_alloc(4 + strlen(result))) == NULL)
		return 0;
	len = strlen(result);
	ptr = GWBUF_DATA(ret);
	*ptr++ = len & 0xff;
	*ptr++ = (len & 0xff00) >> 8;
	*ptr++ = (len & 0xff0000) >> 16;
	*ptr++ = 1;
	strncpy(ptr, result, len);

	return session->dcb->func.write(session->dcb, ret);
}

/**
 * Respond to a COM_PING command
 *
 * @param router	The router instance
 * @param session	The connection that requested the ping
 * @param queue		The ping request
 */
static int
maxinfo_ping(INFO_INSTANCE *router, INFO_SESSION *session, GWBUF *queue)
{
char	*ptr;
GWBUF	*ret;
int	len;

	if ((ret = gwbuf_alloc(5)) == NULL)
		return 0;
	ptr = GWBUF_DATA(ret);
	*ptr++ = 0x01;
	*ptr++ = 0;
	*ptr++ = 0;
	*ptr++ = 1;
	*ptr = 0;		// OK 

	return session->dcb->func.write(session->dcb, ret);
}

/**
 * Populate the version comment with the MaxScale version
 *
 * @param	result	The result set
 * @param	data	Pointer to int which is row count
 * @return	The populated row
 */
static RESULT_ROW *
version_comment(RESULTSET *result, void *data)
{
int		*context = (int *)data;
RESULT_ROW	*row;

	if (*context == 0)
	{
		(*context)++;
		row = resultset_make_row(result);
		resultset_row_set(row, 0, MAXSCALE_VERSION);
		return row;
	}
	return NULL;
}

/**
 * The hardwired select @@vercom response
 * 
 * @param dcb	The DCB of the client
 */
static void
respond_vercom(DCB *dcb)
{
RESULTSET *result;
int	context = 0;

	if ((result = resultset_create(version_comment, &context)) == NULL)
	{
		maxinfo_send_error(dcb, 0, "No resources available");
		return;
	}
	resultset_add_column(result, "@@version_comment", 40, COL_TYPE_VARCHAR);
	resultset_stream_mysql(result, dcb);
	resultset_free(result);
}

/**
 * Populate the version comment with the MaxScale version
 *
 * @param	result	The result set
 * @param	data	Pointer to int which is row count
 * @return	The populated row
 */
static RESULT_ROW *
starttime_row(RESULTSET *result, void *data)
{
int		*context = (int *)data;
RESULT_ROW	*row;
extern time_t	MaxScaleStarted;
struct tm	tm;
static char	buf[40];

	if (*context == 0)
	{
		(*context)++;
		row = resultset_make_row(result);
		sprintf(buf, "%u", MaxScaleStarted);
		resultset_row_set(row, 0, buf);
		return row;
	}
	return NULL;
}

/**
 * The hardwired select ... as starttime response
 * 
 * @param dcb	The DCB of the client
 */
static void
respond_starttime(DCB *dcb)
{
RESULTSET *result;
int	context = 0;

	if ((result = resultset_create(starttime_row, &context)) == NULL)
	{
		maxinfo_send_error(dcb, 0, "No resources available");
		return;
	}
	resultset_add_column(result, "starttime", 40, COL_TYPE_VARCHAR);
	resultset_stream_mysql(result, dcb);
	resultset_free(result);
}

/**
 * Send a MySQL OK packet to the DCB
 *
 * @param dcb	The DCB to send the OK packet to
 * @return result of a write call, non-zero if write was successful
 */
static int
maxinfo_send_ok(DCB *dcb)
{
GWBUF	*buf;
uint8_t *ptr;

	if ((buf = gwbuf_alloc(11)) == NULL)
		return 0;
	ptr = GWBUF_DATA(buf);
	*ptr++ = 7;	// Payload length
	*ptr++ = 0;
	*ptr++ = 0;
	*ptr++ = 1;	// Seqno
	*ptr++ = 0;	// ok
	*ptr++ = 0;
	*ptr++ = 0;
	*ptr++ = 2;
	*ptr++ = 0;
	*ptr++ = 0;
	*ptr++ = 0;
	return dcb->func.write(dcb, buf);
}

/**
 * Execute a SQL query against the MaxScale Information Schema
 *
 * @param instance	The instance strcture
 * @param session	The session pointer
 * @param sql		The SQL to execute
 */
static int
maxinfo_execute_query(INFO_INSTANCE *instance, INFO_SESSION *session, char *sql)
{
MAXINFO_TREE	*tree;
PARSE_ERROR	err;

	LOGIF(LT, (skygw_log_write(LOGFILE_TRACE,
			"maxinfo: SQL statement: '%s' for 0x%x.",
				sql, session->dcb)));
	if (strcmp(sql, "select @@version_comment limit 1") == 0)
	{
		respond_vercom(session->dcb);
		return 1;
	}
	/* Below is a kludge for MonYog, if we see
	 * 	select unix_timestamp... as starttime
	 * just return the starttime of MaxScale
	 */
	if (strncasecmp(sql, "select UNIX_TIMESTAMP",
			strlen("select UNIX_TIMESTAMP")) == 0
				&& strstr(sql, "as starttime") != NULL)
	{
		respond_starttime(session->dcb);
		return 1;
	}
	if (strcasecmp(sql, "set names 'utf8'") == 0)
	{
		return maxinfo_send_ok(session->dcb);
	}
	if (strncasecmp(sql, "set session", 11) == 0)
	{
		return maxinfo_send_ok(session->dcb);
	}
	if (strncasecmp(sql, "SELECT `ENGINES`.`SUPPORT`", 26) == 0)
	{
		return maxinfo_send_ok(session->dcb);
	}
	if ((tree = maxinfo_parse(sql, &err)) == NULL)
	{
		maxinfo_send_parse_error(session->dcb, sql, err);
		LOGIF(LM, (skygw_log_write(
                           LOGFILE_MESSAGE,
			"Failed to parse SQL statement: '%s'.",
			sql)));
	}
	else
		maxinfo_execute(session->dcb, tree);
	return 1;
}