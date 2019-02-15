/* 
   Unix SMB/CIFS implementation.
   Test suite for libnet calls.

   Copyright (C) Rafal Szczesniak 2005
   
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
#include "torture/rpc/rpc.h"
#include "lib/events/events.h"
#include "libnet/libnet.h"
#include "librpc/gen_ndr/ndr_samr_c.h"

static BOOL test_domainopen(struct libnet_context *net_ctx, TALLOC_CTX *mem_ctx,
			    struct lsa_String *domname,
			    struct policy_handle *domain_handle)
{
	NTSTATUS status;
	struct libnet_DomainOpen io;
	
	printf("opening domain\n");
	
	io.in.domain_name  = talloc_strdup(mem_ctx, domname->string);
	io.in.access_mask  = SEC_FLAG_MAXIMUM_ALLOWED;

	status = libnet_DomainOpen(net_ctx, mem_ctx, &io);
	if (!NT_STATUS_IS_OK(status)) {
		printf("Composite domain open failed - %s\n", nt_errstr(status));
		return False;
	}

	*domain_handle = io.out.domain_handle;
	return True;
}


static BOOL test_cleanup(struct dcerpc_pipe *p, TALLOC_CTX *mem_ctx,
			 struct policy_handle *domain_handle)
{
	NTSTATUS status;
	struct samr_Close r;
	struct policy_handle handle;

	r.in.handle   = domain_handle;
	r.out.handle  = &handle;
	
	printf("closing domain handle\n");
	
	status = dcerpc_samr_Close(p, mem_ctx, &r);
	if (!NT_STATUS_IS_OK(status)) {
		printf("Close failed - %s\n", nt_errstr(status));
		return False;
	}
	
	return True;
}


BOOL torture_domainopen(struct torture_context *torture)
{
	NTSTATUS status;
	const char *binding;
	struct libnet_context *net_ctx;
	struct event_context *evt_ctx;
	TALLOC_CTX *mem_ctx;
	BOOL ret = True;
	struct policy_handle h;
	struct lsa_String name;

	mem_ctx = talloc_init("test_domain_open");
	binding = torture_setting_string(torture, "binding", NULL);

	evt_ctx = event_context_find(torture);
	net_ctx = libnet_context_init(evt_ctx);

	status = torture_rpc_connection(mem_ctx, 
					&net_ctx->samr.pipe,
					&dcerpc_table_samr);
	
	if (!NT_STATUS_IS_OK(status)) {
		return False;
	}

	name.string = lp_workgroup();

	/*
	 * Testing synchronous version
	 */
	if (!test_domainopen(net_ctx, mem_ctx, &name, &h)) {
		ret = False;
		goto done;
	}

	if (!test_cleanup(net_ctx->samr.pipe, mem_ctx, &h)) {
		ret = False;
		goto done;
	}

done:
	talloc_free(mem_ctx);

	return ret;
}
