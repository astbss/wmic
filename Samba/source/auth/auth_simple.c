/* 
   Unix SMB/CIFS implementation.

   auth functions

   Copyright (C) Simo Sorce 2005
   Copyright (C) Andrew Tridgell 2005
   Copyright (C) Andrew Bartlett 2005
   
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
#include "auth/auth.h"
#include "lib/events/events.h"

_PUBLIC_ NTSTATUS authenticate_username_pw(TALLOC_CTX *mem_ctx,
					   struct event_context *ev,
					   struct messaging_context *msg,
					   const char *nt4_domain,
					   const char *nt4_username,
					   const char *password,
					   struct auth_session_info **session_info) 
{
	struct auth_context *auth_context;
	struct auth_usersupplied_info *user_info;
	struct auth_serversupplied_info *server_info;
	NTSTATUS nt_status;
	TALLOC_CTX *tmp_ctx = talloc_new(mem_ctx);

	if (!tmp_ctx) {
		return NT_STATUS_NO_MEMORY;
	}

	nt_status = auth_context_create(tmp_ctx, lp_auth_methods(),
					ev, msg,
					&auth_context);
	if (!NT_STATUS_IS_OK(nt_status)) {
		talloc_free(tmp_ctx);
		return nt_status;
	}

	user_info = talloc(tmp_ctx, struct auth_usersupplied_info);
	if (!user_info) {
		talloc_free(tmp_ctx);
		return NT_STATUS_NO_MEMORY;
	}

	user_info->mapped_state = True;
	user_info->client.account_name = nt4_username;
	user_info->mapped.account_name = nt4_username;
	user_info->client.domain_name = nt4_domain;
	user_info->mapped.domain_name = nt4_domain;

	user_info->workstation_name = NULL;

	user_info->remote_host = NULL;

	user_info->password_state = AUTH_PASSWORD_PLAIN;
	user_info->password.plaintext = talloc_strdup(user_info, password);

	user_info->flags = USER_INFO_CASE_INSENSITIVE_USERNAME |
		USER_INFO_DONT_CHECK_UNIX_ACCOUNT;

	user_info->logon_parameters = 0;

	nt_status = auth_check_password(auth_context, tmp_ctx, user_info, &server_info);
	if (!NT_STATUS_IS_OK(nt_status)) {
		talloc_free(tmp_ctx);
		return nt_status;
	}

	nt_status = auth_generate_session_info(tmp_ctx, server_info, session_info);

	if (NT_STATUS_IS_OK(nt_status)) {
		talloc_steal(mem_ctx, *session_info);
	}

	return nt_status;
}

