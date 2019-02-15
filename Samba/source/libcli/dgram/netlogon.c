/* 
   Unix SMB/CIFS implementation.

   handling for netlogon dgram requests

   Copyright (C) Andrew Tridgell 2005
   
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
#include "libcli/dgram/libdgram.h"
#include "lib/socket/socket.h"
#include "libcli/resolve/resolve.h"
#include "librpc/gen_ndr/ndr_nbt.h"

/* 
   send a netlogon mailslot request 
*/
NTSTATUS dgram_mailslot_netlogon_send(struct nbt_dgram_socket *dgmsock,
				      struct nbt_name *dest_name,
				      struct socket_address *dest,
				      struct nbt_name *src_name,
				      struct nbt_netlogon_packet *request)
{
	NTSTATUS status;
	DATA_BLOB blob;
	TALLOC_CTX *tmp_ctx = talloc_new(dgmsock);

	status = ndr_push_struct_blob(&blob, tmp_ctx, request, 
				      (ndr_push_flags_fn_t)ndr_push_nbt_netlogon_packet);
	if (!NT_STATUS_IS_OK(status)) {
		talloc_free(tmp_ctx);
		return status;
	}


	status = dgram_mailslot_send(dgmsock, DGRAM_DIRECT_UNIQUE, 
				     NBT_MAILSLOT_NETLOGON,
				     dest_name, dest, 
				     src_name, &blob);
	talloc_free(tmp_ctx);
	return status;
}


/* 
   send a netlogon mailslot reply
*/
NTSTATUS dgram_mailslot_netlogon_reply(struct nbt_dgram_socket *dgmsock,
				       struct nbt_dgram_packet *request,
				       const char *mailslot_name,
				       struct nbt_netlogon_packet *reply)
{
	NTSTATUS status;
	DATA_BLOB blob;
	TALLOC_CTX *tmp_ctx = talloc_new(dgmsock);
	struct nbt_name myname;
	struct socket_address *dest;

	status = ndr_push_struct_blob(&blob, tmp_ctx, reply, 
				      (ndr_push_flags_fn_t)ndr_push_nbt_netlogon_packet);
	if (!NT_STATUS_IS_OK(status)) {
		talloc_free(tmp_ctx);
		return status;
	}

	make_nbt_name_client(&myname, lp_netbios_name());

	dest = socket_address_from_strings(tmp_ctx, dgmsock->sock->backend_name, 
					   request->src_addr, request->src_port);
	if (!dest) {
		talloc_free(tmp_ctx);
		return NT_STATUS_NO_MEMORY;
	}

	status = dgram_mailslot_send(dgmsock, DGRAM_DIRECT_UNIQUE, 
				     mailslot_name,
				     &request->data.msg.source_name,
				     dest,
				     &myname, &blob);
	talloc_free(tmp_ctx);
	return status;
}


/*
  parse a netlogon response. The packet must be a valid mailslot packet
*/
NTSTATUS dgram_mailslot_netlogon_parse(struct dgram_mailslot_handler *dgmslot,
				       TALLOC_CTX *mem_ctx,
				       struct nbt_dgram_packet *dgram,
				       struct nbt_netlogon_packet *netlogon)
{
	DATA_BLOB data = dgram_mailslot_data(dgram);
	NTSTATUS status;

	status = ndr_pull_struct_blob(&data, mem_ctx, netlogon, 
				      (ndr_pull_flags_fn_t)ndr_pull_nbt_netlogon_packet);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(0,("Failed to parse netlogon packet of length %d\n", 
			 (int)data.length));
		if (DEBUGLVL(10)) {
			file_save("netlogon.dat", data.data, data.length);
		}
	}
	return status;
}
