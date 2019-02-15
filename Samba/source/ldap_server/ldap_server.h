/* 
   Unix SMB/CIFS implementation.
   LDAP server
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

#include "libcli/ldap/ldap.h"
#include "lib/socket/socket.h"
#include "lib/stream/packet.h"

struct ldapsrv_connection {
	struct stream_connection *connection;
	struct gensec_security *gensec;
	struct auth_session_info *session_info;
	struct ldapsrv_service *service;
	struct cli_credentials *server_credentials;
	struct ldb_context *ldb;

	struct {
		struct socket_context *raw;
		struct socket_context *tls;
		struct socket_context *sasl;
	} sockets;

	BOOL global_catalog;

	struct packet_context *packet;

	struct {
		int initial_timeout;
		int conn_idle_time;
		int max_page_size;
		int search_timeout;
		
		struct timed_event *ite;
		struct timed_event *te;
	} limits;
};

struct ldapsrv_call {
	struct ldapsrv_connection *conn;
	struct ldap_message *request;
	struct ldapsrv_reply {
		struct ldapsrv_reply *prev, *next;
		struct ldap_message *msg;
	} *replies;
	packet_send_callback_fn_t send_callback;
	void *send_private;
};

struct ldapsrv_service {
	struct tls_params *tls_params;
};

#include "ldap_server/proto.h"
