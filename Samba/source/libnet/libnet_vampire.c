/* 
   Unix SMB/CIFS implementation.
   
   Extract the user/system database from a remote SamSync server

   Copyright (C) Andrew Bartlett <abartlet@samba.org> 2004-2005
   
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
#include "libnet/libnet.h"
#include "libcli/auth/libcli_auth.h"
#include "auth/gensec/gensec.h"
#include "auth/credentials/credentials.h"
#include "auth/gensec/schannel_proto.h"
#include "librpc/gen_ndr/ndr_netlogon.h"
#include "librpc/gen_ndr/ndr_netlogon_c.h"


/**
 * Decrypt and extract the user's passwords.  
 * 
 * The writes decrypted (no longer 'RID encrypted' or arcfour encrypted) passwords back into the structure
 */
static NTSTATUS fix_user(TALLOC_CTX *mem_ctx,
			 struct creds_CredentialState *creds,
			 enum netr_SamDatabaseID database,
			 struct netr_DELTA_ENUM *delta,
			 char **error_string) 
{

	uint32_t rid = delta->delta_id_union.rid;
	struct netr_DELTA_USER *user = delta->delta_union.user;
	struct samr_Password lm_hash;
	struct samr_Password nt_hash;
	const char *username = user->account_name.string;
	NTSTATUS nt_status;

	if (user->lm_password_present) {
		sam_rid_crypt(rid, user->lmpassword.hash, lm_hash.hash, 0);
		user->lmpassword = lm_hash;
	}

	if (user->nt_password_present) {
		sam_rid_crypt(rid, user->ntpassword.hash, nt_hash.hash, 0);
		user->ntpassword = nt_hash;
	}

	if (user->user_private_info.SensitiveData) {
		DATA_BLOB data;
		struct netr_USER_KEYS keys;
		data.data = user->user_private_info.SensitiveData;
		data.length = user->user_private_info.DataLength;
		creds_arcfour_crypt(creds, data.data, data.length);
		user->user_private_info.SensitiveData = data.data;
		user->user_private_info.DataLength = data.length;

		nt_status = ndr_pull_struct_blob(&data, mem_ctx, &keys, (ndr_pull_flags_fn_t)ndr_pull_netr_USER_KEYS);
		if (NT_STATUS_IS_OK(nt_status)) {
			if (keys.keys.keys2.lmpassword.length == 16) {
				sam_rid_crypt(rid, keys.keys.keys2.lmpassword.pwd.hash, lm_hash.hash, 0);
				user->lmpassword = lm_hash;
				user->lm_password_present = True;
			}
			if (keys.keys.keys2.ntpassword.length == 16) {
				sam_rid_crypt(rid, keys.keys.keys2.ntpassword.pwd.hash, nt_hash.hash, 0);
				user->ntpassword = nt_hash;
				user->nt_password_present = True;
			}
		} else {
			*error_string = talloc_asprintf(mem_ctx, "Failed to parse Sensitive Data for %s:", username);
			dump_data(10, data.data, data.length);
			return nt_status;
		}
	}
	return NT_STATUS_OK;
}

/**
 * Decrypt and extract the secrets
 * 
 * The writes decrypted secrets back into the structure
 */
static NTSTATUS fix_secret(TALLOC_CTX *mem_ctx,
			   struct creds_CredentialState *creds,
			   enum netr_SamDatabaseID database,
			   struct netr_DELTA_ENUM *delta,
			   char **error_string) 
{
	struct netr_DELTA_SECRET *secret = delta->delta_union.secret;
	creds_arcfour_crypt(creds, secret->current_cipher.cipher_data, 
			    secret->current_cipher.maxlen); 

	creds_arcfour_crypt(creds, secret->old_cipher.cipher_data, 
			    secret->old_cipher.maxlen); 

	return NT_STATUS_OK;
}

/**
 * Fix up the delta, dealing with encryption issues so that the final
 * callback need only do the printing or application logic
 */

static NTSTATUS fix_delta(TALLOC_CTX *mem_ctx, 		
			  struct creds_CredentialState *creds,
			  enum netr_SamDatabaseID database,
			  struct netr_DELTA_ENUM *delta,
			  char **error_string)
{
	NTSTATUS nt_status = NT_STATUS_OK;
	*error_string = NULL;
	switch (delta->delta_type) {
	case NETR_DELTA_USER:
	{
		nt_status = fix_user(mem_ctx, 
				     creds,
				     database,
				     delta,
				     error_string);
		break;
	}
	case NETR_DELTA_SECRET:
	{
		nt_status = fix_secret(mem_ctx, 
				       creds,
				       database,
				       delta,
				       error_string);
		break;
	}
	default:
		break;
	}
	return nt_status;
}

NTSTATUS libnet_SamSync_netlogon(struct libnet_context *ctx, TALLOC_CTX *mem_ctx, struct libnet_SamSync *r)
{
	NTSTATUS nt_status, dbsync_nt_status;
	TALLOC_CTX *samsync_ctx, *loop_ctx, *delta_ctx;
	struct creds_CredentialState *creds;
	struct netr_DatabaseSync dbsync;
	struct cli_credentials *machine_account;
	struct dcerpc_pipe *p;
	struct libnet_context *machine_net_ctx;
	struct libnet_RpcConnect *c;
	struct libnet_SamSync_state *state;
	const enum netr_SamDatabaseID database_ids[] = {SAM_DATABASE_DOMAIN, SAM_DATABASE_BUILTIN, SAM_DATABASE_PRIVS}; 
	int i;

	samsync_ctx = talloc_named(mem_ctx, 0, "SamSync top context");

	if (!r->in.machine_account) { 
		machine_account = cli_credentials_init(samsync_ctx);
		if (!machine_account) {
			talloc_free(samsync_ctx);
			return NT_STATUS_NO_MEMORY;
		}
		cli_credentials_set_conf(machine_account);
		nt_status = cli_credentials_set_machine_account(machine_account);
		if (!NT_STATUS_IS_OK(nt_status)) {
			r->out.error_string = talloc_strdup(mem_ctx, "Could not obtain machine account password - are we joined to the domain?");
			talloc_free(samsync_ctx);
			return nt_status;
		}
	} else {
		machine_account = r->in.machine_account;
	}

	/* We cannot do this unless we are a BDC.  Check, before we get odd errors later */
	if (cli_credentials_get_secure_channel_type(machine_account) != SEC_CHAN_BDC) {
		r->out.error_string
			= talloc_asprintf(mem_ctx, 
					  "Our join to domain %s is not as a BDC (%d), please rejoin as a BDC",
					  cli_credentials_get_domain(machine_account),
					  cli_credentials_get_secure_channel_type(machine_account));
		talloc_free(samsync_ctx);
		return NT_STATUS_CANT_ACCESS_DOMAIN_INFO;
	}

	c = talloc(samsync_ctx, struct libnet_RpcConnect);
	if (!c) {
		r->out.error_string = NULL;
		talloc_free(samsync_ctx);
		return NT_STATUS_NO_MEMORY;
	}

	c->level              = LIBNET_RPC_CONNECT_DC_INFO;
	if (r->in.binding_string) {
		c->in.binding = r->in.binding_string;
		c->in.name    = NULL;
	} else {
		c->in.binding = NULL;
		c->in.name    = cli_credentials_get_domain(machine_account);
	}
	
	/* prepare connect to the NETLOGON pipe of PDC */
	c->in.dcerpc_iface      = &dcerpc_table_netlogon;

	/* We must do this as the machine, not as any command-line
	 * user.  So we override the credentials in the
	 * libnet_context */
	machine_net_ctx = talloc(samsync_ctx, struct libnet_context);
	if (!machine_net_ctx) {
		r->out.error_string = NULL;
		talloc_free(samsync_ctx);
		return NT_STATUS_NO_MEMORY;
	}
	*machine_net_ctx = *ctx;
	machine_net_ctx->cred = machine_account;

	/* connect to the NETLOGON pipe of the PDC */
	nt_status = libnet_RpcConnect(machine_net_ctx, samsync_ctx, c);
	if (!NT_STATUS_IS_OK(nt_status)) {
		if (r->in.binding_string) {
			r->out.error_string = talloc_asprintf(mem_ctx,
							      "Connection to NETLOGON pipe of DC %s failed: %s",
							      r->in.binding_string, c->out.error_string);
		} else {
			r->out.error_string = talloc_asprintf(mem_ctx,
							      "Connection to NETLOGON pipe of DC for %s failed: %s",
							      c->in.name, c->out.error_string);
		}
		talloc_free(samsync_ctx);
		return nt_status;
	}

	/* This makes a new pipe, on which we can do schannel.  We
	 * should do this in the RpcConnect code, but the abstaction
	 * layers do not suit yet */

	nt_status = dcerpc_secondary_connection(c->out.dcerpc_pipe, &p,
						c->out.dcerpc_pipe->binding);

	if (!NT_STATUS_IS_OK(nt_status)) {
		r->out.error_string = talloc_asprintf(mem_ctx,
						      "Secondary connection to NETLOGON pipe of DC %s failed: %s",
						      dcerpc_server_name(p), nt_errstr(nt_status));
		talloc_free(samsync_ctx);
		return nt_status;
	}

	nt_status = dcerpc_bind_auth_schannel(samsync_ctx, p, &dcerpc_table_netlogon,
					      machine_account, DCERPC_AUTH_LEVEL_PRIVACY);

	if (!NT_STATUS_IS_OK(nt_status)) {
		r->out.error_string = talloc_asprintf(mem_ctx,
						      "SCHANNEL authentication to NETLOGON pipe of DC %s failed: %s",
						      dcerpc_server_name(p), nt_errstr(nt_status));
		talloc_free(samsync_ctx);
		return nt_status;
	}

	state = talloc(samsync_ctx, struct libnet_SamSync_state);
	if (!state) {
		r->out.error_string = NULL;
		talloc_free(samsync_ctx);
		return nt_status;
	}		

	state->domain_name     = c->out.domain_name;
	state->domain_sid      = c->out.domain_sid;
	state->realm           = c->out.realm;
	state->domain_guid     = c->out.guid;
	state->machine_net_ctx = machine_net_ctx;
	state->netlogon_pipe   = p;

	/* initialise the callback layer.  It may wish to contact the
	 * server with ldap, now we know the name */
	
	if (r->in.init_fn) {
		char *error_string;
		nt_status = r->in.init_fn(samsync_ctx, 
					  r->in.fn_ctx,
					  state, 
					  &error_string); 
		if (!NT_STATUS_IS_OK(nt_status)) {
			r->out.error_string = talloc_steal(mem_ctx, error_string);
			talloc_free(samsync_ctx);
			return nt_status;
		}
	}

	/* get NETLOGON credentails */

	nt_status = dcerpc_schannel_creds(p->conn->security_state.generic_state, samsync_ctx, &creds);
	if (!NT_STATUS_IS_OK(nt_status)) {
		r->out.error_string = talloc_strdup(mem_ctx, "Could not obtain NETLOGON credentials from DCERPC/GENSEC layer");
		talloc_free(samsync_ctx);
		return nt_status;
	}

	/* Setup details for the synchronisation */
	dbsync.in.logon_server = talloc_asprintf(samsync_ctx, "\\\\%s", dcerpc_server_name(p));
	dbsync.in.computername = cli_credentials_get_workstation(machine_account);
	dbsync.in.preferredmaximumlength = (uint32_t)-1;
	ZERO_STRUCT(dbsync.in.return_authenticator);

	for (i=0;i< ARRAY_SIZE(database_ids); i++) { 
		dbsync.in.sync_context = 0;
		dbsync.in.database_id = database_ids[i]; 
		
		do {
			int d;
			loop_ctx = talloc_named(samsync_ctx, 0, "DatabaseSync loop context");
			creds_client_authenticator(creds, &dbsync.in.credential);
			
			dbsync_nt_status = dcerpc_netr_DatabaseSync(p, loop_ctx, &dbsync);
			if (!NT_STATUS_IS_OK(dbsync_nt_status) &&
			    !NT_STATUS_EQUAL(dbsync_nt_status, STATUS_MORE_ENTRIES)) {
				r->out.error_string = talloc_asprintf(mem_ctx, "DatabaseSync failed - %s", nt_errstr(nt_status));
				talloc_free(samsync_ctx);
				return nt_status;
			}
			
			if (!creds_client_check(creds, &dbsync.out.return_authenticator.cred)) {
				r->out.error_string = talloc_strdup(mem_ctx, "Credential chaining on incoming DatabaseSync failed");
				talloc_free(samsync_ctx);
				return NT_STATUS_ACCESS_DENIED;
			}
			
			dbsync.in.sync_context = dbsync.out.sync_context;
			
			/* For every single remote 'delta' entry: */
			for (d=0; d < dbsync.out.delta_enum_array->num_deltas; d++) {
				char *error_string = NULL;
				delta_ctx = talloc_named(loop_ctx, 0, "DatabaseSync delta context");
				/* 'Fix' elements, by decrypting and
				 * de-obfuscating the data */
				nt_status = fix_delta(delta_ctx, 
						      creds, 
						      dbsync.in.database_id,
						      &dbsync.out.delta_enum_array->delta_enum[d], 
						      &error_string);
				if (!NT_STATUS_IS_OK(nt_status)) {
					r->out.error_string = talloc_steal(mem_ctx, error_string);
					talloc_free(samsync_ctx);
					return nt_status;
				}

				/* Now call the callback.  This will
				 * do something like print the data or
				 * write to an ldb */
				nt_status = r->in.delta_fn(delta_ctx, 
							   r->in.fn_ctx,
							   dbsync.in.database_id,
							   &dbsync.out.delta_enum_array->delta_enum[d], 
							   &error_string);
				if (!NT_STATUS_IS_OK(nt_status)) {
					r->out.error_string = talloc_steal(mem_ctx, error_string);
					talloc_free(samsync_ctx);
					return nt_status;
				}
				talloc_free(delta_ctx);
			}
			talloc_free(loop_ctx);
		} while (NT_STATUS_EQUAL(dbsync_nt_status, STATUS_MORE_ENTRIES));
		
		if (!NT_STATUS_IS_OK(dbsync_nt_status)) {
			r->out.error_string = talloc_asprintf(mem_ctx, "libnet_SamSync_netlogon failed: unexpected inconsistancy. Should not get error %s here", nt_errstr(nt_status));
			talloc_free(samsync_ctx);
			return dbsync_nt_status;
		}
		nt_status = NT_STATUS_OK;
	}
	talloc_free(samsync_ctx);
	return nt_status;
}

