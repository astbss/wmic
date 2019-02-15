/* 
   Unix SMB/CIFS implementation.
   
   Copyright (C) Stefan Metzmacher      2004
   Copyright (C) Rafal Szczesniak       2005-2006
   
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

struct libnet_context {
	/* here we need:
	 * a client env context
	 * a user env context
	 */
	struct cli_credentials *cred;

	/* samr connection parameters - opened handles and related properties */
	struct {
		struct dcerpc_pipe *pipe;
		const char *name;
		uint32_t access_mask;
		struct policy_handle handle;
	} samr;

	/* lsa connection parameters - opened handles and related properties */
	struct {
		struct dcerpc_pipe *pipe;
		const char *name;
		uint32_t access_mask;
		struct policy_handle handle;
	} lsa;

	/* name resolution methods */
	const char **name_res_methods;

	struct event_context *event_ctx;
};


#include "lib/ldb/include/ldb.h"
#include "libnet/composite.h"
#include "libnet/userman.h"
#include "libnet/userinfo.h"
#include "libnet/libnet_passwd.h"
#include "libnet/libnet_time.h"
#include "libnet/libnet_rpc.h"
#include "libnet/libnet_join.h"
#include "libnet/libnet_site.h"
#include "libnet/libnet_become_dc.h"
#include "libnet/libnet_unbecome_dc.h"
#include "libnet/libnet_vampire.h"
#include "libnet/libnet_user.h"
#include "libnet/libnet_share.h"
#include "libnet/libnet_lookup.h"
#include "libnet/libnet_domain.h"
#include "libnet/libnet_proto.h"
