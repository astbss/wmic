/* 
   Unix SMB/CIFS implementation.

   helper functions for stream based servers

   Copyright (C) Andrew Tridgell 2003-2005
   Copyright (C) Stefan (metze) Metzmacher	2004
   
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
#include "process_model.h"
#include "lib/events/events.h"
#include "lib/socket/socket.h"
#include "smbd/service.h"
#include "smbd/service_stream.h"
#include "lib/messaging/irpc.h"

/* the range of ports to try for dcerpc over tcp endpoints */
#define SERVER_TCP_LOW_PORT  1024
#define SERVER_TCP_HIGH_PORT 1300

/* size of listen() backlog in smbd */
#define SERVER_LISTEN_BACKLOG 10


/*
  private structure for a single listening stream socket
*/
struct stream_socket {
	const struct stream_server_ops *ops;
	struct event_context *event_ctx;
	const struct model_ops *model_ops;
	struct socket_context *sock;
	void *private;
};


/*
  close the socket and shutdown a stream_connection
*/
void stream_terminate_connection(struct stream_connection *srv_conn, const char *reason)
{
	struct event_context *event_ctx = srv_conn->event.ctx;
	const struct model_ops *model_ops = srv_conn->model_ops;

	if (!reason) reason = "unknown reason";

	srv_conn->terminate = reason;

	if (srv_conn->processing) {
		/* 
		 * if we're currently inside the stream_io_handler(),
		 * defer the termination to the end of stream_io_hendler()
		 *
		 * and we don't want to read or write to the connection...
		 */
		event_set_fd_flags(srv_conn->event.fde, 0);
		return;
	}

	talloc_free(srv_conn->event.fde);
	srv_conn->event.fde = NULL;
	talloc_free(srv_conn);
	model_ops->terminate(event_ctx, reason);
}

/*
  the select loop has indicated that a stream is ready for IO
*/
static void stream_io_handler(struct stream_connection *conn, uint16_t flags)
{
	conn->processing = True;
	if (flags & EVENT_FD_WRITE) {
		conn->ops->send_handler(conn, flags);
	} else if (flags & EVENT_FD_READ) {
		conn->ops->recv_handler(conn, flags);
	}
	conn->processing = False;

	if (conn->terminate) {
		stream_terminate_connection(conn, conn->terminate);
	}
}

static void stream_io_handler_fde(struct event_context *ev, struct fd_event *fde, 
				  uint16_t flags, void *private)
{
	struct stream_connection *conn = talloc_get_type(private, 
							 struct stream_connection);
	stream_io_handler(conn, flags);
}

void stream_io_handler_callback(void *private, uint16_t flags) 
{
	struct stream_connection *conn = talloc_get_type(private, 
							 struct stream_connection);
	stream_io_handler(conn, flags);
}

/*
  this creates a stream_connection from an already existing connection,
  used for protocols, where a client connection needs to switched into
  a server connection
*/
NTSTATUS stream_new_connection_merge(struct event_context *ev,
				     const struct model_ops *model_ops,
				     struct socket_context *sock,
				     const struct stream_server_ops *stream_ops,
				     struct messaging_context *msg_ctx,
				     void *private_data,
				     struct stream_connection **_srv_conn)
{
	struct stream_connection *srv_conn;

	srv_conn = talloc_zero(ev, struct stream_connection);
	NT_STATUS_HAVE_NO_MEMORY(srv_conn);

	talloc_steal(srv_conn, sock);

	srv_conn->private       = private_data;
	srv_conn->model_ops     = model_ops;
	srv_conn->socket	= sock;
	srv_conn->server_id	= 0;
	srv_conn->ops           = stream_ops;
	srv_conn->msg_ctx	= msg_ctx;
	srv_conn->event.ctx	= ev;
	srv_conn->event.fde	= event_add_fd(ev, srv_conn, socket_get_fd(sock),
					       EVENT_FD_READ, 
					       stream_io_handler_fde, srv_conn);
	*_srv_conn = srv_conn;
	return NT_STATUS_OK;
}

/*
  called when a new socket connection has been established. This is called in the process
  context of the new process (if appropriate)
*/
static void stream_new_connection(struct event_context *ev,
				  struct socket_context *sock, 
				  uint32_t server_id, void *private)
{
	struct stream_socket *stream_socket = talloc_get_type(private, struct stream_socket);
	struct stream_connection *srv_conn;
	struct socket_address *c, *s;

	srv_conn = talloc_zero(ev, struct stream_connection);
	if (!srv_conn) {
		DEBUG(0,("talloc(mem_ctx, struct stream_connection) failed\n"));
		return;
	}

	talloc_steal(srv_conn, sock);

	srv_conn->private       = stream_socket->private;
	srv_conn->model_ops     = stream_socket->model_ops;
	srv_conn->socket	= sock;
	srv_conn->server_id	= server_id;
	srv_conn->ops           = stream_socket->ops;
	srv_conn->event.ctx	= ev;
	srv_conn->event.fde	= event_add_fd(ev, srv_conn, socket_get_fd(sock),
					       EVENT_FD_READ, 
					       stream_io_handler_fde, srv_conn);

	if (!socket_check_access(sock, "smbd", lp_hostsallow(-1), lp_hostsdeny(-1))) {
		stream_terminate_connection(srv_conn, "denied by access rules");
		return;
	}

	/* setup to receive internal messages on this connection */
	srv_conn->msg_ctx = messaging_init(srv_conn, srv_conn->server_id, ev);
	if (!srv_conn->msg_ctx) {
		stream_terminate_connection(srv_conn, "messaging_init() failed");
		return;
	}

	c = socket_get_peer_addr(sock, ev);
	s = socket_get_my_addr(sock, ev);
	if (s && c) {
		const char *title;
		title = talloc_asprintf(s, "conn[%s] c[%s:%u] s[%s:%u] server_id[%d]",
					stream_socket->ops->name, 
					c->addr, c->port, s->addr, s->port,
					server_id);
		if (title) {
			stream_connection_set_title(srv_conn, title);
		}
	}
	talloc_free(c);
	talloc_free(s);

	/* call the server specific accept code */
	stream_socket->ops->accept_connection(srv_conn);
}


/*
  called when someone opens a connection to one of our listening ports
*/
static void stream_accept_handler(struct event_context *ev, struct fd_event *fde, 
				  uint16_t flags, void *private)
{
	struct stream_socket *stream_socket = talloc_get_type(private, struct stream_socket);

	/* ask the process model to create us a process for this new
	   connection.  When done, it calls stream_new_connection()
	   with the newly created socket */
	stream_socket->model_ops->accept_connection(ev, stream_socket->sock, 
						    stream_new_connection, stream_socket);
}



/*
  setup a listen stream socket
  if you pass *port == 0, then a port > 1024 is used
 */
NTSTATUS stream_setup_socket(struct event_context *event_context,
			     const struct model_ops *model_ops,
			     const struct stream_server_ops *stream_ops,
			     const char *family,
			     const char *sock_addr,
			     uint16_t *port,
			     void *private)
{
	NTSTATUS status;
	struct stream_socket *stream_socket;
	struct socket_address *socket_address;
	int i;

	stream_socket = talloc_zero(event_context, struct stream_socket);
	NT_STATUS_HAVE_NO_MEMORY(stream_socket);

	status = socket_create(family, SOCKET_TYPE_STREAM, &stream_socket->sock, 0);
	NT_STATUS_NOT_OK_RETURN(status);

	talloc_steal(stream_socket, stream_socket->sock);

	/* ready to listen */
	status = socket_set_option(stream_socket->sock, "SO_KEEPALIVE", NULL);
	NT_STATUS_NOT_OK_RETURN(status);

	status = socket_set_option(stream_socket->sock, lp_socket_options(), NULL);
	NT_STATUS_NOT_OK_RETURN(status);

	/* TODO: set socket ACL's here when they're implemented */

	if (*port == 0) {
		for (i=SERVER_TCP_LOW_PORT;i<= SERVER_TCP_HIGH_PORT;i++) {
			socket_address = socket_address_from_strings(stream_socket, 
								     stream_socket->sock->backend_name,
								     sock_addr, i);
			NT_STATUS_HAVE_NO_MEMORY(socket_address);
			status = socket_listen(stream_socket->sock, socket_address, 
					       SERVER_LISTEN_BACKLOG, 0);
			talloc_free(socket_address);
			if (NT_STATUS_IS_OK(status)) {
				*port = i;
				break;
			}
		}
	} else {
		socket_address = socket_address_from_strings(stream_socket, 
							     stream_socket->sock->backend_name,
							     sock_addr, *port);
		NT_STATUS_HAVE_NO_MEMORY(socket_address);
		status = socket_listen(stream_socket->sock, socket_address, SERVER_LISTEN_BACKLOG, 0);
		talloc_free(socket_address);
	}

	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(0,("Failed to listen on %s:%u - %s\n",
			sock_addr, *port, nt_errstr(status)));
		talloc_free(stream_socket);
		return status;
	}

	event_add_fd(event_context, stream_socket->sock, 
		     socket_get_fd(stream_socket->sock), 
		     EVENT_FD_READ, stream_accept_handler, stream_socket);

	stream_socket->private          = talloc_reference(stream_socket, private);
	stream_socket->ops              = stream_ops;
	stream_socket->event_ctx	= event_context;
	stream_socket->model_ops        = model_ops;

	return NT_STATUS_OK;
}

/*
  setup a connection title 
*/
void stream_connection_set_title(struct stream_connection *conn, const char *title)
{
	conn->model_ops->set_title(conn->event.ctx, title);
}
