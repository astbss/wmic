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
#include "lib/cmdline/popt_common.h"
#include "libnet/libnet.h"
#include "librpc/gen_ndr/nbt.h"
#include "librpc/rpc/dcerpc.h"
#include "libcli/libcli.h"
#include "torture/torture.h"


BOOL torture_lookup(struct torture_context *torture)
{
	BOOL ret;
	NTSTATUS status;
	TALLOC_CTX *mem_ctx;
	struct libnet_context *ctx;
	struct libnet_Lookup lookup;
	struct dcerpc_binding *bind;
	const char *bindstr;

	mem_ctx = talloc_init("test_lookup");

	ctx = libnet_context_init(NULL);
	ctx->cred = cmdline_credentials;

	lookup.in.hostname = torture_setting_string(torture, "host", NULL);
	if (lookup.in.hostname == NULL) {
		bindstr = torture_setting_string(torture, "binding", NULL);
		status = dcerpc_parse_binding(mem_ctx, bindstr, &bind);
		if (NT_STATUS_IS_OK(status)) {
			lookup.in.hostname = bind->host;
		}
	}

	lookup.in.type     = NBT_NAME_CLIENT;
	lookup.in.methods  = NULL;
	lookup.out.address = NULL;

	status = libnet_Lookup(ctx, mem_ctx, &lookup);

	if (!NT_STATUS_IS_OK(status)) {
		printf("Couldn't lookup name %s: %s\n", lookup.in.hostname, nt_errstr(status));
		ret = False;
		goto done;
	}

	ret = True;

	printf("Name [%s] found at adrress: %s.\n", lookup.in.hostname, *lookup.out.address);

done:
	talloc_free(mem_ctx);
	return ret;
}


BOOL torture_lookup_host(struct torture_context *torture)
{
	BOOL ret;
	NTSTATUS status;
	TALLOC_CTX *mem_ctx;
	struct libnet_context *ctx;
	struct libnet_Lookup lookup;
	struct dcerpc_binding *bind;
	const char *bindstr;

	mem_ctx = talloc_init("test_lookup_host");

	ctx = libnet_context_init(NULL);
	ctx->cred = cmdline_credentials;

	lookup.in.hostname = torture_setting_string(torture, "host", NULL);
	if (lookup.in.hostname == NULL) {
		bindstr = torture_setting_string(torture, "binding", NULL);
		status = dcerpc_parse_binding(mem_ctx, bindstr, &bind);
		if (NT_STATUS_IS_OK(status)) {
			lookup.in.hostname = bind->host;
		}
	}

	lookup.in.methods  = NULL;
	lookup.out.address = NULL;

	status = libnet_LookupHost(ctx, mem_ctx, &lookup);

	if (!NT_STATUS_IS_OK(status)) {
		printf("Couldn't lookup host %s: %s\n", lookup.in.hostname, nt_errstr(status));
		ret = False;
		goto done;
	}

	ret = True;

	printf("Host [%s] found at adrress: %s.\n", lookup.in.hostname, *lookup.out.address);

done:
	talloc_free(mem_ctx);
	return ret;
}


BOOL torture_lookup_pdc(struct torture_context *torture)
{
	BOOL ret;
	NTSTATUS status;
	TALLOC_CTX *mem_ctx;
	struct libnet_context *ctx;
	struct libnet_LookupDCs *lookup;
	int i;

	mem_ctx = talloc_init("test_lookup_pdc");

	ctx = libnet_context_init(NULL);
	ctx->cred = cmdline_credentials;

	talloc_steal(ctx, mem_ctx);

	lookup = talloc(mem_ctx, struct libnet_LookupDCs);
	if (!lookup) {
		ret = False;
		goto done;
	}

	lookup->in.domain_name = lp_workgroup();
	lookup->in.name_type   = NBT_NAME_PDC;

	status = libnet_LookupDCs(ctx, mem_ctx, lookup);

	if (!NT_STATUS_IS_OK(status)) {
		printf("Couldn't lookup pdc %s: %s\n", lookup->in.domain_name,
		       nt_errstr(status));
		ret = False;
		goto done;
	}

	ret = True;

	printf("DCs of domain [%s] found.\n", lookup->in.domain_name);
	for (i = 0; i < lookup->out.num_dcs; i++) {
		printf("\tDC[%d]: name=%s, address=%s\n", i, lookup->out.dcs[i].name,
		       lookup->out.dcs[i].address);
	}

done:
	talloc_free(mem_ctx);
	return ret;
}


BOOL torture_lookup_sam_name(struct torture_context *torture)
{
	NTSTATUS status;
	TALLOC_CTX *mem_ctx;
	struct libnet_context *ctx;
	struct libnet_LookupName r;

	ctx = libnet_context_init(NULL);
	ctx->cred = cmdline_credentials;

	mem_ctx = talloc_init("torture lookup sam name");
	if (mem_ctx == NULL) return False;

	r.in.name = "Administrator";
	r.in.domain_name = lp_workgroup();

	status = libnet_LookupName(ctx, mem_ctx, &r);

	talloc_free(mem_ctx);
	talloc_free(ctx);

	return True;
}
