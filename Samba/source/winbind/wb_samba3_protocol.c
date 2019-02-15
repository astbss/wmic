/* 
   Unix SMB/CIFS implementation.
   Main winbindd samba3 server routines

   Copyright (C) Stefan Metzmacher	2005
   Copyright (C) Volker Lendecke	2005

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
#include "nsswitch/winbindd_nss.h"
#include "winbind/wb_server.h"
#include "smbd/service_stream.h"
#include "lib/stream/packet.h"

/*
  work out if a packet is complete for protocols that use a 32 bit host byte
  order length
*/
NTSTATUS wbsrv_samba3_packet_full_request(void *private, DATA_BLOB blob, size_t *size)
{
	uint32_t *len;
	if (blob.length < 4) {
		return STATUS_MORE_ENTRIES;
	}
	len = (uint32_t *)blob.data;
	*size = (*len);
	if (*size > blob.length) {
		return STATUS_MORE_ENTRIES;
	}
	return NT_STATUS_OK;
}


NTSTATUS wbsrv_samba3_pull_request(DATA_BLOB blob, struct wbsrv_connection *wbconn,
				   struct wbsrv_samba3_call **_call)
{
	struct wbsrv_samba3_call *call;

	if (blob.length != sizeof(call->request)) {
		DEBUG(0,("wbsrv_samba3_pull_request: invalid blob length %lu should be %lu\n"
			 " make sure you use the correct winbind client tools!\n",
			 (long)blob.length, (long)sizeof(call->request)));
		return NT_STATUS_INVALID_PARAMETER;
	}

	call = talloc_zero(wbconn, struct wbsrv_samba3_call);
	NT_STATUS_HAVE_NO_MEMORY(call);

	/* the packet layout is the same as the in memory layout of the request, so just copy it */
	memcpy(&call->request, blob.data, sizeof(call->request));

	call->wbconn = wbconn;
	call->event_ctx = call->wbconn->conn->event.ctx;
	
	*_call = call;
	return NT_STATUS_OK;
}

NTSTATUS wbsrv_samba3_handle_call(struct wbsrv_samba3_call *s3call)
{
	DEBUG(10, ("Got winbind samba3 request %d\n", s3call->request.cmd));

	s3call->response.length = sizeof(s3call->response);

	switch(s3call->request.cmd) {
	case WINBINDD_INTERFACE_VERSION:
		return wbsrv_samba3_interface_version(s3call);

#if 0
	case WINBINDD_CHECK_MACHACC:
		return wbsrv_samba3_check_machacc(s3call);
#endif

	case WINBINDD_PING:
		return wbsrv_samba3_ping(s3call);

	case WINBINDD_INFO:
		return wbsrv_samba3_info(s3call);

	case WINBINDD_DOMAIN_NAME:
		return wbsrv_samba3_domain_name(s3call);

	case WINBINDD_NETBIOS_NAME:
		return wbsrv_samba3_netbios_name(s3call);

	case WINBINDD_PRIV_PIPE_DIR:
		return wbsrv_samba3_priv_pipe_dir(s3call);

	case WINBINDD_LOOKUPNAME:
		return wbsrv_samba3_lookupname(s3call);

	case WINBINDD_LOOKUPSID:
		return wbsrv_samba3_lookupsid(s3call);

	case WINBINDD_PAM_AUTH:
		return wbsrv_samba3_pam_auth(s3call);

	case WINBINDD_PAM_AUTH_CRAP:
		return wbsrv_samba3_pam_auth_crap(s3call);

	case WINBINDD_GETDCNAME:
		return wbsrv_samba3_getdcname(s3call);

	case WINBINDD_GETUSERDOMGROUPS:
		return wbsrv_samba3_userdomgroups(s3call);

	case WINBINDD_GETUSERSIDS:
		return wbsrv_samba3_usersids(s3call);

	case WINBINDD_LIST_TRUSTDOM:
		return wbsrv_samba3_list_trustdom(s3call);

		/* Unimplemented commands */

	case WINBINDD_GETPWNAM:
	case WINBINDD_GETPWUID:
	case WINBINDD_GETGRNAM:
	case WINBINDD_GETGRGID:
	case WINBINDD_GETGROUPS:
	case WINBINDD_SETPWENT:
	case WINBINDD_ENDPWENT:
	case WINBINDD_GETPWENT:
	case WINBINDD_SETGRENT:
	case WINBINDD_ENDGRENT:
	case WINBINDD_GETGRENT:
	case WINBINDD_PAM_CHAUTHTOK:
	case WINBINDD_LIST_USERS:
	case WINBINDD_LIST_GROUPS:
	case WINBINDD_SID_TO_UID:
	case WINBINDD_SID_TO_GID:
	case WINBINDD_UID_TO_SID:
	case WINBINDD_GID_TO_SID:
	case WINBINDD_ALLOCATE_RID:
	case WINBINDD_ALLOCATE_RID_AND_GID:
	case WINBINDD_CHECK_MACHACC:
	case WINBINDD_DOMAIN_INFO:
	case WINBINDD_SHOW_SEQUENCE:
	case WINBINDD_WINS_BYIP:
	case WINBINDD_WINS_BYNAME:
	case WINBINDD_GETGRLST:
	case WINBINDD_INIT_CONNECTION:
	case WINBINDD_DUAL_SID2UID:
	case WINBINDD_DUAL_SID2GID:
	case WINBINDD_DUAL_IDMAPSET:
	case WINBINDD_DUAL_UID2NAME:
	case WINBINDD_DUAL_NAME2UID:
	case WINBINDD_DUAL_GID2NAME:
	case WINBINDD_DUAL_NAME2GID:
	case WINBINDD_DUAL_USERINFO:
	case WINBINDD_DUAL_GETSIDALIASES:
	case WINBINDD_NUM_CMDS:
		DEBUG(10, ("Unimplemented winbind samba3 request %d\n", 
			   s3call->request.cmd));
		break;
	}

	s3call->response.result = WINBINDD_ERROR;
	return NT_STATUS_OK;
}

static NTSTATUS wbsrv_samba3_push_reply(struct wbsrv_samba3_call *call, TALLOC_CTX *mem_ctx, DATA_BLOB *_blob)
{
	DATA_BLOB blob;
	uint8_t *extra_data;
	size_t extra_data_len = 0;

	extra_data = call->response.extra_data;
	if (extra_data) {
		extra_data_len = call->response.length -
			sizeof(call->response);
	}

	blob = data_blob_talloc(mem_ctx, NULL, call->response.length);
	NT_STATUS_HAVE_NO_MEMORY(blob.data);

	/* don't push real pointer values into sockets */
	if (extra_data) {
		call->response.extra_data = (void *)0xFFFFFFFF;
	}
	memcpy(blob.data, &call->response, sizeof(call->response));
	/* set back the pointer */
	call->response.extra_data = extra_data;

	if (extra_data) {
		memcpy(blob.data + sizeof(call->response), extra_data, extra_data_len);
	}

	*_blob = blob;
	return NT_STATUS_OK;
}

/*
 * queue a wbsrv_call reply on a wbsrv_connection
 * NOTE: that this implies talloc_free(call),
 *       use talloc_reference(call) if you need it after
 *       calling wbsrv_queue_reply
 */
NTSTATUS wbsrv_samba3_send_reply(struct wbsrv_samba3_call *call)
{
	struct wbsrv_connection *wbconn = call->wbconn;
	DATA_BLOB rep;
	NTSTATUS status;

	status = wbsrv_samba3_push_reply(call, call, &rep);
	NT_STATUS_NOT_OK_RETURN(status);

	status = packet_send(call->wbconn->packet, rep);
	
	talloc_free(call);

	if (!NT_STATUS_IS_OK(status)) {
		wbsrv_terminate_connection(wbconn,
					   "failed to packet_send winbindd reply");
		return status;
	}
	/* the call isn't needed any more */
	return status;
}

NTSTATUS wbsrv_samba3_process(void *private, DATA_BLOB blob)
{
	NTSTATUS status;
	struct wbsrv_connection *wbconn = talloc_get_type(private, 
							  struct wbsrv_connection);
	struct wbsrv_samba3_call *call;
	status = wbsrv_samba3_pull_request(blob, wbconn, &call);
	
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}
	
	status = wbsrv_samba3_handle_call(call);

	if (!NT_STATUS_IS_OK(status)) {
		talloc_free(call);
		return status;
	}

	if (call->flags & WBSRV_CALL_FLAGS_REPLY_ASYNC) {
		return NT_STATUS_OK;
	}

	status = wbsrv_samba3_send_reply(call);
	return status;
}

