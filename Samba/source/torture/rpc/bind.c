/* 
   Unix SMB/CIFS implementation.

   dcerpc torture tests

   Copyright (C) Andrew Tridgell 2003
   Copyright (C) Andrew Bartlett <abartlet@samba.org 2004

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
#include "torture/torture.h"
#include "librpc/gen_ndr/ndr_lsa.h"
#include "librpc/gen_ndr/ndr_lsa_c.h"
#include "lib/cmdline/popt_common.h"
#include "librpc/rpc/dcerpc.h"
#include "torture/rpc/rpc.h"
#include "libcli/libcli.h"
#include "libcli/composite/composite.h"
#include "libcli/smb_composite/smb_composite.h"

/*
  This test is 'bogus' in that it doesn't actually perform to the
  spec.  We need to deal with other things inside the DCERPC layer,
  before we could have multiple binds.

  We should never pass this test, until such details are fixed in our
  client, and it looks like multible binds are never used anyway.

*/

BOOL torture_multi_bind(struct torture_context *torture) 
{
	struct dcerpc_pipe *p;
	struct dcerpc_binding *binding;
	const char *binding_string = torture_setting_string(torture, "binding", NULL);
	TALLOC_CTX *mem_ctx;
	NTSTATUS status;
	BOOL ret;

	mem_ctx = talloc_init("torture_multi_bind");

	status = dcerpc_parse_binding(mem_ctx, binding_string, &binding);
	if (!NT_STATUS_IS_OK(status)) {
		printf("Failed to parse dcerpc binding '%s'\n", binding_string);
		talloc_free(mem_ctx);
		return False;
	}

	status = torture_rpc_connection(mem_ctx, &p, &dcerpc_table_lsarpc);
	
	if (!NT_STATUS_IS_OK(status)) {
		talloc_free(mem_ctx);
		return False;
	}

	status = dcerpc_pipe_auth(mem_ctx, &p, binding, &dcerpc_table_lsarpc, cmdline_credentials);

	if (NT_STATUS_IS_OK(status)) {
		printf("(incorrectly) allowed re-bind to uuid %s - %s\n", 
			GUID_string(mem_ctx, &dcerpc_table_lsarpc.syntax_id.uuid), nt_errstr(status));
		ret = False;
	} else {
		printf("\n");
		ret = True;
	}

	talloc_free(mem_ctx);

	return ret;
}
