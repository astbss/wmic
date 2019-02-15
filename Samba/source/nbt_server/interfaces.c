/* 
   Unix SMB/CIFS implementation.

   NBT interface handling

   Copyright (C) Andrew Tridgell	2005
   
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
#include "lib/util/dlinklist.h"
#include "nbt_server/nbt_server.h"
#include "smbd/service_task.h"
#include "lib/socket/socket.h"
#include "nbt_server/wins/winsserver.h"
#include "nbt_server/dgram/proto.h"
#include "system/network.h"
#include "lib/socket/netif.h"


/*
  receive an incoming request and dispatch it to the right place
*/
static void nbtd_request_handler(struct nbt_name_socket *nbtsock, 
				 struct nbt_name_packet *packet, 
				 struct socket_address *src)
{
	struct nbtd_interface *iface = talloc_get_type(nbtsock->incoming.private, 
						       struct nbtd_interface);
	struct nbtd_server *nbtsrv = iface->nbtsrv;

	nbtsrv->stats.total_received++;

	/* see if its from one of our own interfaces - if so, then ignore it */
	if (nbtd_self_packet_and_bcast(nbtsock, packet, src)) {
		DEBUG(10,("Ignoring bcast self packet from %s:%d\n", src->addr, src->port));
		return;
	}

	switch (packet->operation & NBT_OPCODE) {
	case NBT_OPCODE_QUERY:
		nbtsrv->stats.query_count++;
		nbtd_request_query(nbtsock, packet, src);
		break;

	case NBT_OPCODE_REGISTER:
	case NBT_OPCODE_REFRESH:
	case NBT_OPCODE_REFRESH2:
		nbtsrv->stats.register_count++;
		nbtd_request_defense(nbtsock, packet, src);
		break;

	case NBT_OPCODE_RELEASE:
	case NBT_OPCODE_MULTI_HOME_REG:
		nbtsrv->stats.release_count++;
		nbtd_winsserver_request(nbtsock, packet, src);
		break;

	default:
		nbtd_bad_packet(packet, src, "Unexpected opcode");
		break;
	}
}


/*
  find a registered name on an interface
*/
struct nbtd_iface_name *nbtd_find_iname(struct nbtd_interface *iface, 
					struct nbt_name *name, 
					uint16_t nb_flags)
{
	struct nbtd_iface_name *iname;
	for (iname=iface->names;iname;iname=iname->next) {
		if (iname->name.type == name->type &&
		    strcmp(name->name, iname->name.name) == 0 &&
		    ((iname->nb_flags & nb_flags) == nb_flags)) {
			return iname;
		}
	}
	return NULL;
}

/*
  start listening on the given address
*/
static NTSTATUS nbtd_add_socket(struct nbtd_server *nbtsrv, 
				const char *bind_address, 
				const char *address, 
				const char *bcast, 
				const char *netmask)
{
	struct nbtd_interface *iface;
	NTSTATUS status;
	struct socket_address *bcast_address;
	struct socket_address *unicast_address;

	/*
	  we actually create two sockets. One listens on the broadcast address
	  for the interface, and the other listens on our specific address. This
	  allows us to run with "bind interfaces only" while still receiving 
	  broadcast addresses, and also simplifies matching incoming requests 
	  to interfaces
	*/

	iface = talloc(nbtsrv, struct nbtd_interface);
	NT_STATUS_HAVE_NO_MEMORY(iface);

	iface->nbtsrv        = nbtsrv;
	iface->bcast_address = talloc_steal(iface, bcast);
	iface->ip_address    = talloc_steal(iface, address);
	iface->netmask       = talloc_steal(iface, netmask);
	iface->names         = NULL;

	if (strcmp(netmask, "0.0.0.0") != 0) {
		struct nbt_name_socket *bcast_nbtsock;

		/* listen for broadcasts on port 137 */
		bcast_nbtsock = nbt_name_socket_init(iface, nbtsrv->task->event_ctx);
		if (!bcast_nbtsock) {
			talloc_free(iface);
			return NT_STATUS_NO_MEMORY;
		}

		bcast_address = socket_address_from_strings(bcast_nbtsock, bcast_nbtsock->sock->backend_name, 
							    bcast, lp_nbt_port());
		if (!bcast_address) {
			talloc_free(iface);
			return NT_STATUS_NO_MEMORY;
		}

		status = socket_listen(bcast_nbtsock->sock, bcast_address, 0, 0);
		if (!NT_STATUS_IS_OK(status)) {
			DEBUG(0,("Failed to bind to %s:%d - %s\n", 
				 bcast, lp_nbt_port(), nt_errstr(status)));
			talloc_free(iface);
			return status;
		}
		talloc_free(bcast_address);

		nbt_set_incoming_handler(bcast_nbtsock, nbtd_request_handler, iface);
	}

	/* listen for unicasts on port 137 */
	iface->nbtsock = nbt_name_socket_init(iface, nbtsrv->task->event_ctx);
	if (!iface->nbtsock) {
		talloc_free(iface);
		return NT_STATUS_NO_MEMORY;
	}

	unicast_address = socket_address_from_strings(iface->nbtsock, iface->nbtsock->sock->backend_name, 
						      bind_address, lp_nbt_port());

	status = socket_listen(iface->nbtsock->sock, unicast_address, 0, 0);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(0,("Failed to bind to %s:%d - %s\n", 
			 bind_address, lp_nbt_port(), nt_errstr(status)));
		talloc_free(iface);
		return status;
	}
	talloc_free(unicast_address);

	nbt_set_incoming_handler(iface->nbtsock, nbtd_request_handler, iface);

	/* also setup the datagram listeners */
	status = nbtd_dgram_setup(iface, bind_address);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(0,("Failed to setup dgram listen on %s - %s\n", 
			 bind_address, nt_errstr(status)));
		talloc_free(iface);
		return status;
	}
	
	if (strcmp(netmask, "0.0.0.0") == 0) {
		DLIST_ADD(nbtsrv->bcast_interface, iface);
	} else {
		DLIST_ADD(nbtsrv->interfaces, iface);
	}

	return NT_STATUS_OK;
}

/*
  setup a socket for talking to our WINS servers
*/
static NTSTATUS nbtd_add_wins_socket(struct nbtd_server *nbtsrv)
{
	struct nbtd_interface *iface;

	iface = talloc_zero(nbtsrv, struct nbtd_interface);
	NT_STATUS_HAVE_NO_MEMORY(iface);

	iface->nbtsrv        = nbtsrv;

	DLIST_ADD(nbtsrv->wins_interface, iface);

	return NT_STATUS_OK;
}


/*
  setup our listening sockets on the configured network interfaces
*/
NTSTATUS nbtd_startup_interfaces(struct nbtd_server *nbtsrv)
{
	int num_interfaces = iface_count();
	int i;
	TALLOC_CTX *tmp_ctx = talloc_new(nbtsrv);
	NTSTATUS status;

	/* if we are allowing incoming packets from any address, then
	   we also need to bind to the wildcard address */
	if (!lp_bind_interfaces_only()) {
		const char *primary_address;

		/* the primary address is the address we will return
		   for non-WINS queries not made on a specific
		   interface */
		if (num_interfaces > 0) {
			primary_address = iface_n_ip(0);
		} else {
			primary_address = sys_inet_ntoa(interpret_addr2(
								lp_netbios_name()));
		}
		primary_address = talloc_strdup(tmp_ctx, primary_address);
		NT_STATUS_HAVE_NO_MEMORY(primary_address);

		status = nbtd_add_socket(nbtsrv, 
					 "0.0.0.0",
					 primary_address,
					 talloc_strdup(tmp_ctx, "255.255.255.255"),
					 talloc_strdup(tmp_ctx, "0.0.0.0"));
		NT_STATUS_NOT_OK_RETURN(status);
	}

	for (i=0; i<num_interfaces; i++) {
		const char *bcast = iface_n_bcast(i);
		const char *address, *netmask;

		/* we can't assume every interface is broadcast capable */
		if (bcast == NULL) continue;

		address = talloc_strdup(tmp_ctx, iface_n_ip(i));
		bcast   = talloc_strdup(tmp_ctx, bcast);
		netmask = talloc_strdup(tmp_ctx, iface_n_netmask(i));

		status = nbtd_add_socket(nbtsrv, address, address, bcast, netmask);
		NT_STATUS_NOT_OK_RETURN(status);
	}

	if (lp_wins_server_list()) {
		status = nbtd_add_wins_socket(nbtsrv);
		NT_STATUS_NOT_OK_RETURN(status);
	}

	talloc_free(tmp_ctx);

	return NT_STATUS_OK;
}


/*
  form a list of addresses that we should use in name query replies
  we always place the IP in the given interface first
*/
const char **nbtd_address_list(struct nbtd_interface *iface, TALLOC_CTX *mem_ctx)
{
	struct nbtd_server *nbtsrv = iface->nbtsrv;
	const char **ret = NULL;
	struct nbtd_interface *iface2;
	BOOL is_loopback = False;

	if (iface->ip_address) {
		is_loopback = iface_same_net(iface->ip_address, "127.0.0.1", "255.0.0.0");
		ret = str_list_add(ret, iface->ip_address);
	}

	for (iface2=nbtsrv->interfaces;iface2;iface2=iface2->next) {
		if (iface2 == iface) continue;

		if (!iface2->ip_address) continue;

		if (!is_loopback) {
			if (iface_same_net(iface2->ip_address, "127.0.0.1", "255.0.0.0")) {
				continue;
			}
		}

		ret = str_list_add(ret, iface2->ip_address);
	}

	talloc_steal(mem_ctx, ret);

	return ret;
}


/*
  find the interface to use for sending a outgoing request
*/
struct nbtd_interface *nbtd_find_request_iface(struct nbtd_server *nbtd_server,
					       const char *address, BOOL allow_bcast_iface)
{
	struct nbtd_interface *cur;

	/* try to find a exact match */
	for (cur=nbtd_server->interfaces;cur;cur=cur->next) {
		if (iface_same_net(address, cur->ip_address, cur->netmask)) {
			return cur;
		}
	}

	/* no exact match, if we have the broadcast interface, use that */
	if (allow_bcast_iface && nbtd_server->bcast_interface) {
		return nbtd_server->bcast_interface;
	}

	/* fallback to first interface */
	return nbtd_server->interfaces;
}

/*
 * find the interface to use for sending a outgoing reply
 */
struct nbtd_interface *nbtd_find_reply_iface(struct nbtd_interface *iface,
					     const char *address, BOOL allow_bcast_iface)
{
	struct nbtd_server *nbtd_server = iface->nbtsrv;

	/* first try to use the given interfacel when it's not the broadcast one */
	if (iface != nbtd_server->bcast_interface) {
		return iface;
	}

	return nbtd_find_request_iface(nbtd_server, address, allow_bcast_iface);
}
