/* 
   Unix SMB/CIFS implementation.

   LDAP server

   Copyright (C) Andrew Tridgell 2005
   Copyright (C) Volker Lendecke 2004
   Copyright (C) Stefan Metzmacher 2004
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "includes.h"
#include "lib/events/events.h"
#include "auth/auth.h"
#include "auth/credentials/credentials.h"
#include "librpc/gen_ndr/ndr_samr.h"
#include "lib/util/dlinklist.h"
#include "libcli/util/asn_1.h"
#include "ldap_server/ldap_server.h"
#include "smbd/service_task.h"
#include "smbd/service_stream.h"
#include "smbd/service.h"
#include "smbd/process_model.h"
#include "lib/tls/tls.h"
#include "lib/messaging/irpc.h"
#include "lib/ldb/include/ldb.h"
#include "lib/ldb/include/ldb_errors.h"
#include "system/network.h"
#include "lib/socket/netif.h"

/*
  close the socket and shutdown a server_context
*/
void ldapsrv_terminate_connection(struct ldapsrv_connection *conn, 
					 const char *reason)
{
	stream_terminate_connection(conn->connection, reason);
}

/*
  handle packet errors
*/
static void ldapsrv_error_handler(void *private, NTSTATUS status)
{
	struct ldapsrv_connection *conn = talloc_get_type(private, 
							  struct ldapsrv_connection);
	ldapsrv_terminate_connection(conn, nt_errstr(status));
}

/*
  process a decoded ldap message
*/
static void ldapsrv_process_message(struct ldapsrv_connection *conn,
				    struct ldap_message *msg)
{
	struct ldapsrv_call *call;
	NTSTATUS status;
	DATA_BLOB blob;

	call = talloc(conn, struct ldapsrv_call);
	if (!call) {
		ldapsrv_terminate_connection(conn, "no memory");
		return;		
	}
	
	call->request = talloc_steal(call, msg);
	call->conn = conn;
	call->replies = NULL;
	call->send_callback = NULL;
	call->send_private = NULL;
	
	/* make the call */
	status = ldapsrv_do_call(call);
	if (!NT_STATUS_IS_OK(status)) {
		talloc_free(call);
		return;
	}
	
	blob = data_blob(NULL, 0);

	if (call->replies == NULL) {
		talloc_free(call);
		return;
	}

	/* build all the replies into a single blob */
	while (call->replies) {
		DATA_BLOB b;

		msg = call->replies->msg;
		if (!ldap_encode(msg, &b, call)) {
			DEBUG(0,("Failed to encode ldap reply of type %d\n", msg->type));
			talloc_free(call);
			return;
		}

		status = data_blob_append(call, &blob, b.data, b.length);
		data_blob_free(&b);

		talloc_set_name_const(blob.data, "Outgoing, encoded LDAP packet");

		if (!NT_STATUS_IS_OK(status)) {
			talloc_free(call);
			return;
		}

		DLIST_REMOVE(call->replies, call->replies);
	}

	packet_send_callback(conn->packet, blob, 
			     call->send_callback, call->send_private);
	talloc_free(call);
	return;
}

/*
  decode/process data
*/
static NTSTATUS ldapsrv_decode(void *private, DATA_BLOB blob)
{
	struct ldapsrv_connection *conn = talloc_get_type(private, 
							  struct ldapsrv_connection);
	struct asn1_data asn1;
	struct ldap_message *msg = talloc(conn, struct ldap_message);

	if (msg == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	if (!asn1_load(&asn1, blob)) {
		return NT_STATUS_NO_MEMORY;
	}

	if (!ldap_decode(&asn1, msg)) {
		asn1_free(&asn1);
		return NT_STATUS_LDAP(LDAP_PROTOCOL_ERROR);
	}

	data_blob_free(&blob);
	ldapsrv_process_message(conn, msg);
	asn1_free(&asn1);
	return NT_STATUS_OK;
}

/*
 Idle timeout handler
*/
static void ldapsrv_conn_idle_timeout(struct event_context *ev,
				      struct timed_event *te,
				      struct timeval t,
				      void *private)
{
	struct ldapsrv_connection *conn = talloc_get_type(private, struct ldapsrv_connection);

	ldapsrv_terminate_connection(conn, "Timeout. No requests after bind");
}

/*
  called when a LDAP socket becomes readable
*/
void ldapsrv_recv(struct stream_connection *c, uint16_t flags)
{
	struct ldapsrv_connection *conn = 
		talloc_get_type(c->private, struct ldapsrv_connection);

	if (conn->limits.ite) { /* clean initial timeout if any */
		talloc_free(conn->limits.ite);
		conn->limits.ite = NULL;
	}

	if (conn->limits.te) { /* clean idle timeout if any */
		talloc_free(conn->limits.te);
		conn->limits.te = NULL;
	}

	packet_recv(conn->packet);

	/* set idle timeout */
	conn->limits.te = event_add_timed(c->event.ctx, conn, 
					   timeval_current_ofs(conn->limits.conn_idle_time, 0),
					   ldapsrv_conn_idle_timeout, conn);
}

/*
  called when a LDAP socket becomes writable
*/
static void ldapsrv_send(struct stream_connection *c, uint16_t flags)
{
	struct ldapsrv_connection *conn = 
		talloc_get_type(c->private, struct ldapsrv_connection);
	
	packet_queue_run(conn->packet);
}

static void ldapsrv_conn_init_timeout(struct event_context *ev,
				      struct timed_event *te,
				      struct timeval t,
				      void *private)
{
	struct ldapsrv_connection *conn = talloc_get_type(private, struct ldapsrv_connection);

	ldapsrv_terminate_connection(conn, "Timeout. No requests after initial connection");
}

static int ldapsrv_load_limits(struct ldapsrv_connection *conn)
{
	TALLOC_CTX *tmp_ctx;
	const char *attrs[] = { "configurationNamingContext", NULL };
	const char *attrs2[] = { "lDAPAdminLimits", NULL };
	struct ldb_message_element *el;
	struct ldb_result *res = NULL;
	struct ldb_dn *basedn;
	struct ldb_dn *conf_dn;
	struct ldb_dn *policy_dn;
	int i,ret;

	/* set defaults limits in case of failure */
	conn->limits.initial_timeout = 120;
	conn->limits.conn_idle_time = 900;
	conn->limits.max_page_size = 1000;
	conn->limits.search_timeout = 120;


	tmp_ctx = talloc_new(conn);
	if (tmp_ctx == NULL) {
		return -1;
	}

	basedn = ldb_dn_new(tmp_ctx, conn->ldb, NULL);
	if ( ! ldb_dn_validate(basedn)) {
		goto failed;
	}

	ret = ldb_search(conn->ldb, basedn, LDB_SCOPE_BASE, NULL, attrs, &res);
	talloc_steal(tmp_ctx, res);
	if (ret != LDB_SUCCESS || res->count != 1) {
		goto failed;
	}

	conf_dn = ldb_msg_find_attr_as_dn(conn->ldb, tmp_ctx, res->msgs[0], "configurationNamingContext");
	if (conf_dn == NULL) {
		goto failed;
	}

	policy_dn = ldb_dn_copy(tmp_ctx, conf_dn);
	ldb_dn_add_child_fmt(policy_dn, "CN=Default Query Policy,CN=Query-Policies,CN=Directory Service,CN=Windows NT,CN=Services");
	if (policy_dn == NULL) {
		goto failed;
	}

	ret = ldb_search(conn->ldb, policy_dn, LDB_SCOPE_BASE, NULL, attrs2, &res);
	talloc_steal(tmp_ctx, res);
	if (ret != LDB_SUCCESS || res->count != 1) {
		goto failed;
	}

	el = ldb_msg_find_element(res->msgs[0], "lDAPAdminLimits");
	if (el == NULL) {
		goto failed;
	}

	for (i = 0; i < el->num_values; i++) {
		char policy_name[256];
		int policy_value, s;

		s = sscanf((const char *)el->values[i].data, "%255[^=]=%d", policy_name, &policy_value);
		if (ret != 2 || policy_value == 0)
			continue;
		
		if (strcasecmp("InitRecvTimeout", policy_name) == 0) {
			conn->limits.initial_timeout = policy_value;
			continue;
		}
		if (strcasecmp("MaxConnIdleTime", policy_name) == 0) {
			conn->limits.conn_idle_time = policy_value;
			continue;
		}
		if (strcasecmp("MaxPageSize", policy_name) == 0) {
			conn->limits.max_page_size = policy_value;
			continue;
		}
		if (strcasecmp("MaxQueryDuration", policy_name) == 0) {
			conn->limits.search_timeout = policy_value;
			continue;
		}
	}

	return 0;

failed:
	DEBUG(0, ("Failed to load ldap server query policies\n"));
	talloc_free(tmp_ctx);
	return -1;
}

/*
  initialise a server_context from a open socket and register a event handler
  for reading from that socket
*/
static void ldapsrv_accept(struct stream_connection *c)
{
	struct ldapsrv_service *ldapsrv_service = 
		talloc_get_type(c->private, struct ldapsrv_service);
	struct ldapsrv_connection *conn;
	struct cli_credentials *server_credentials;
	struct socket_address *socket_address;
	NTSTATUS status;
	int port;

	conn = talloc_zero(c, struct ldapsrv_connection);
	if (!conn) {
		stream_terminate_connection(c, "ldapsrv_accept: out of memory");
		return;
	}

	conn->packet      = NULL;
	conn->connection  = c;
	conn->service     = ldapsrv_service;
	conn->sockets.raw = c->socket;

	c->private        = conn;

	socket_address = socket_get_my_addr(c->socket, conn);
	if (!socket_address) {
		ldapsrv_terminate_connection(conn, "ldapsrv_accept: failed to obtain local socket address!");
		return;
	}
	port = socket_address->port;
	talloc_free(socket_address);

	if (port == 636) {
		struct socket_context *tls_socket = tls_init_server(ldapsrv_service->tls_params, c->socket, 
								    c->event.fde, NULL);
		if (!tls_socket) {
			ldapsrv_terminate_connection(conn, "ldapsrv_accept: tls_init_server() failed");
			return;
		}
		talloc_unlink(c, c->socket);
		talloc_steal(c, tls_socket);
		c->socket = tls_socket;
		conn->sockets.tls = tls_socket;

	} else if (port == 3268) /* Global catalog */ {
		conn->global_catalog = True;
	}
	conn->packet = packet_init(conn);
	if (conn->packet == NULL) {
		ldapsrv_terminate_connection(conn, "out of memory");
		return;
	}

	packet_set_private(conn->packet, conn);
	packet_set_socket(conn->packet, c->socket);
	packet_set_callback(conn->packet, ldapsrv_decode);
	packet_set_full_request(conn->packet, ldap_full_packet);
	packet_set_error_handler(conn->packet, ldapsrv_error_handler);
	packet_set_event_context(conn->packet, c->event.ctx);
	packet_set_fde(conn->packet, c->event.fde);
	packet_set_serialise(conn->packet);
	
	/* Ensure we don't get packets until the database is ready below */
	packet_recv_disable(conn->packet);

	server_credentials 
		= cli_credentials_init(conn);
	if (!server_credentials) {
		stream_terminate_connection(c, "Failed to init server credentials\n");
		return;
	}
	
	cli_credentials_set_conf(server_credentials);
	status = cli_credentials_set_machine_account(server_credentials);
	if (!NT_STATUS_IS_OK(status)) {
		stream_terminate_connection(c, talloc_asprintf(conn, "Failed to obtain server credentials, perhaps a standalone server?: %s\n", nt_errstr(status)));
		return;
	}
	conn->server_credentials = server_credentials;

	/* Connections start out anonymous */
	if (!NT_STATUS_IS_OK(auth_anonymous_session_info(conn, &conn->session_info))) {
		ldapsrv_terminate_connection(conn, "failed to setup anonymous session info");
		return;
	}

	if (!NT_STATUS_IS_OK(ldapsrv_backend_Init(conn))) {
		ldapsrv_terminate_connection(conn, "backend Init failed");
		return;
	}

	/* load limits from the conf partition */
	ldapsrv_load_limits(conn); /* should we fail on error ? */

	/* register the server */	
	irpc_add_name(c->msg_ctx, "ldap_server");

	/* set connections limits */
	conn->limits.ite = event_add_timed(c->event.ctx, conn, 
					   timeval_current_ofs(conn->limits.initial_timeout, 0),
					   ldapsrv_conn_init_timeout, conn);

	packet_recv_enable(conn->packet);

}

static const struct stream_server_ops ldap_stream_ops = {
	.name			= "ldap",
	.accept_connection	= ldapsrv_accept,
	.recv_handler		= ldapsrv_recv,
	.send_handler		= ldapsrv_send,
};

/*
  add a socket address to the list of events, one event per port
*/
static NTSTATUS add_socket(struct event_context *event_context,
			   const struct model_ops *model_ops,
			   const char *address, struct ldapsrv_service *ldap_service)
{
	uint16_t port = 389;
	NTSTATUS status;

	status = stream_setup_socket(event_context, model_ops, &ldap_stream_ops, 
				     "ipv4", address, &port, ldap_service);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(0,("ldapsrv failed to bind to %s:%u - %s\n",
			 address, port, nt_errstr(status)));
	}

	if (tls_support(ldap_service->tls_params)) {
		/* add ldaps server */
		port = 636;
		status = stream_setup_socket(event_context, model_ops, &ldap_stream_ops, 
					     "ipv4", address, &port, ldap_service);
		if (!NT_STATUS_IS_OK(status)) {
			DEBUG(0,("ldapsrv failed to bind to %s:%u - %s\n",
				 address, port, nt_errstr(status)));
		}
	}

	/* if we are a PDC, then also enable the global catalog server port, 3268 */
	if (lp_server_role() == ROLE_DOMAIN_PDC) {
		port = 3268;
		status = stream_setup_socket(event_context, model_ops, &ldap_stream_ops, 
					     "ipv4", address, &port, ldap_service);
		if (!NT_STATUS_IS_OK(status)) {
			DEBUG(0,("ldapsrv failed to bind to %s:%u - %s\n",
				 address, port, nt_errstr(status)));
		}
	}

	return status;
}

/*
  open the ldap server sockets
*/
static void ldapsrv_task_init(struct task_server *task)
{	
	struct ldapsrv_service *ldap_service;
	NTSTATUS status;
	const struct model_ops *model_ops;

	task_server_set_title(task, "task[ldapsrv]");

	/* run the ldap server as a single process */
	model_ops = process_model_byname("single");
	if (!model_ops) goto failed;

	ldap_service = talloc_zero(task, struct ldapsrv_service);
	if (ldap_service == NULL) goto failed;

	ldap_service->tls_params = tls_initialise(ldap_service);
	if (ldap_service->tls_params == NULL) goto failed;

	if (lp_interfaces() && lp_bind_interfaces_only()) {
		int num_interfaces = iface_count();
		int i;

		/* We have been given an interfaces line, and been 
		   told to only bind to those interfaces. Create a
		   socket per interface and bind to only these.
		*/
		for(i = 0; i < num_interfaces; i++) {
			const char *address = iface_n_ip(i);
			status = add_socket(task->event_ctx, model_ops, address, ldap_service);
			if (!NT_STATUS_IS_OK(status)) goto failed;
		}
	} else {
		status = add_socket(task->event_ctx, model_ops, lp_socket_address(), ldap_service);
		if (!NT_STATUS_IS_OK(status)) goto failed;
	}

	return;

failed:
	task_server_terminate(task, "Failed to startup ldap server task");	
}

/*
  called on startup of the web server service It's job is to start
  listening on all configured sockets
*/
static NTSTATUS ldapsrv_init(struct event_context *event_context, 
			     const struct model_ops *model_ops)
{	
	return task_server_startup(event_context, model_ops, ldapsrv_task_init);
}


NTSTATUS server_service_ldap_init(void)
{
	return register_server_service("ldap", ldapsrv_init);
}
