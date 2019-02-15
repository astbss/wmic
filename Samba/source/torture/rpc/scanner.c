/* 
   Unix SMB/CIFS implementation.

   scanner for rpc calls

   Copyright (C) Andrew Tridgell 2003
   
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
#include "librpc/gen_ndr/ndr_mgmt_c.h"
#include "librpc/rpc/dcerpc_table.h"
#include "torture/rpc/rpc.h"

/*
  work out how many calls there are for an interface
 */
static BOOL test_num_calls(const struct dcerpc_interface_table *iface,
			   TALLOC_CTX *mem_ctx,
			   struct dcerpc_syntax_id *id)
{
	struct dcerpc_pipe *p;
	NTSTATUS status;
	int i;
	DATA_BLOB stub_in, stub_out;
	int idl_calls;
	struct dcerpc_interface_table tbl;

	/* FIXME: This should be fixed when torture_rpc_connection 
	 * takes a dcerpc_syntax_id */
	tbl.name = iface->name;
	tbl.syntax_id = *id;

	status = torture_rpc_connection(mem_ctx, &p, iface);
	if (!NT_STATUS_IS_OK(status)) {
		char *uuid_str = GUID_string(mem_ctx, &id->uuid);
		printf("Failed to connect to '%s' on '%s' - %s\n", 
		       uuid_str, iface->name, nt_errstr(status));
		talloc_free(uuid_str);
		return False;
	}

	/* make null calls */
	stub_in = data_blob(NULL, 1000);
	memset(stub_in.data, 0xFF, stub_in.length);

	for (i=0;i<200;i++) {
		status = dcerpc_request(p, NULL, False, i, mem_ctx, &stub_in, &stub_out);
		if (!NT_STATUS_IS_OK(status) &&
		    p->last_fault_code == DCERPC_FAULT_OP_RNG_ERROR) {
			break;
		}

		if (!NT_STATUS_IS_OK(status) && p->last_fault_code == 5) {
			printf("\tpipe disconnected at %d\n", i);
			goto done;
		}

		if (!NT_STATUS_IS_OK(status) && p->last_fault_code == 0x80010111) {
			printf("\terr 0x80010111 at %d\n", i);
			goto done;
		}
	}

	printf("\t%d calls available\n", i);
	idl_calls = idl_num_calls(&id->uuid, id->if_version);
	if (idl_calls == -1) {
		printf("\tinterface not known in local IDL\n");
	} else if (i != idl_calls) {
		printf("\tWARNING: local IDL defines %u calls\n", idl_calls);
	} else {
		printf("\tOK: matches num_calls in local IDL\n");
	}

done:
	talloc_free(p);
	return True;
}

/*
  ask the server what interface IDs are available on this endpoint
*/
static BOOL test_inq_if_ids(struct dcerpc_pipe *p, 
			    TALLOC_CTX *mem_ctx,
			    const struct dcerpc_interface_table *iface)
{
	NTSTATUS status;
	struct mgmt_inq_if_ids r;
	int i;
	
	status = dcerpc_mgmt_inq_if_ids(p, mem_ctx, &r);
	if (!NT_STATUS_IS_OK(status)) {
		printf("inq_if_ids failed - %s\n", nt_errstr(status));
		return False;
	}

	if (!W_ERROR_IS_OK(r.out.result)) {
		printf("inq_if_ids gave error code %s\n", win_errstr(r.out.result));
		return False;
	}

	if (!r.out.if_id_vector) {
		printf("inq_if_ids gave NULL if_id_vector\n");
		return False;
	}

	for (i=0;i<r.out.if_id_vector->count;i++) {
		const char *uuid;
		struct dcerpc_syntax_id *id = r.out.if_id_vector->if_id[i].id;
		if (!id) continue;

		uuid = GUID_string(mem_ctx, &id->uuid),

		printf("\n\tuuid %s  version 0x%08x '%s'\n",
		       uuid,
		       id->if_version, idl_pipe_name(&id->uuid, id->if_version));

		test_num_calls(iface, mem_ctx, id);
	}

	return True;
}


BOOL torture_rpc_scanner(struct torture_context *torture)
{
        NTSTATUS status;
        struct dcerpc_pipe *p;
	TALLOC_CTX *mem_ctx, *loop_ctx;
	BOOL ret = True;
	const struct dcerpc_interface_list *l;
	const char *binding = torture_setting_string(torture, "binding", NULL);
	struct dcerpc_binding *b;

	mem_ctx = talloc_init("torture_rpc_scanner");

	if (!binding) {
		talloc_free(mem_ctx);
		printf("You must supply a ncacn binding string\n");
		return False;
	}
	
	status = dcerpc_parse_binding(mem_ctx, binding, &b);
	if (!NT_STATUS_IS_OK(status)) {
		talloc_free(mem_ctx);
		printf("Failed to parse binding '%s'\n", binding);
		return False;
	}

	for (l=librpc_dcerpc_pipes();l;l=l->next) {		
		loop_ctx = talloc_named(mem_ctx, 0, "torture_rpc_scanner loop context");
		/* some interfaces are not mappable */
		if (l->table->num_calls == 0 ||
		    strcmp(l->table->name, "mgmt") == 0) {
			talloc_free(loop_ctx);
			continue;
		}

		printf("\nTesting pipe '%s'\n", l->table->name);

		if (b->transport == NCACN_IP_TCP) {
			status = dcerpc_epm_map_binding(mem_ctx, b, l->table, NULL);
			if (!NT_STATUS_IS_OK(status)) {
				printf("Failed to map port for uuid %s\n", 
					   GUID_string(loop_ctx, &l->table->syntax_id.uuid));
				talloc_free(loop_ctx);
				continue;
			}
		} else {
			b->endpoint = talloc_strdup(b, l->table->name);
		}

		lp_set_cmdline("torture:binding", dcerpc_binding_string(mem_ctx, b));

		status = torture_rpc_connection(loop_ctx, &p, &dcerpc_table_mgmt);
		if (!NT_STATUS_IS_OK(status)) {
			talloc_free(loop_ctx);
			ret = False;
			continue;
		}
	
		if (!test_inq_if_ids(p, mem_ctx, l->table)) {
			ret = False;
		}
	}

	return ret;
}
