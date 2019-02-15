/*
   WMI Sample client
   Copyright (C) 2006 Andrzej Hajda <andrzej.hajda@wp.pl>

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
#include "auth/credentials/credentials.h"
#include "librpc/rpc/dcerpc.h"
#include "librpc/gen_ndr/ndr_oxidresolver.h"
#include "librpc/gen_ndr/ndr_oxidresolver_c.h"
#include "librpc/gen_ndr/dcom.h"
#include "librpc/gen_ndr/ndr_dcom.h"
#include "librpc/gen_ndr/ndr_dcom_c.h"
#include "librpc/gen_ndr/ndr_remact_c.h"
#include "librpc/gen_ndr/ndr_epmapper_c.h"
#include "librpc/gen_ndr/com_dcom.h"
#include "librpc/rpc/dcerpc_table.h"

#include "lib/com/dcom/dcom.h"
#include "lib/com/proto.h"
#include "lib/com/dcom/proto.h"

#include "wmi/wmi.h"

#include "wmi/proto.h"

struct program_args {
    char *hostname;
    char *query;
};

static void parse_args(int argc, char *argv[], struct program_args *pmyargs)
{
    poptContext pc;
    int opt, i;

    int argc_new;
    char **argv_new;

    struct poptOption long_options[] = {
	POPT_AUTOHELP
	POPT_COMMON_SAMBA
	POPT_COMMON_CONNECTION
	POPT_COMMON_CREDENTIALS
	POPT_COMMON_VERSION
	POPT_TABLEEND
    };

    pc = poptGetContext("wmi", argc, (const char **) argv,
	        long_options, POPT_CONTEXT_KEEP_FIRST);

    poptSetOtherOptionHelp(pc, "//host\n\nExample: wmis -U [domain/]adminuser%password //host");

    while ((opt = poptGetNextOpt(pc)) != -1) {
	poptPrintUsage(pc, stdout, 0);
	poptFreeContext(pc);
	exit(1);
    }

    argv_new = discard_const_p(char *, poptGetArgs(pc));

    argc_new = argc;
    for (i = 0; i < argc; i++) {
	if (argv_new[i] == NULL) {
	    argc_new = i;
	    break;
	}
    }

    if (argc_new < 2 || argv_new[1][0] != '/'
        || argv_new[1][1] != '/') {
	poptPrintUsage(pc, stdout, 0);
	poptFreeContext(pc);
	exit(1);
    }

    pmyargs->hostname = argv_new[1] + 2;
    poptFreeContext(pc);
}

#define WERR_CHECK(msg) if (!W_ERROR_IS_OK(result)) { \
			    DEBUG(0, ("ERROR: %s\n", msg)); \
			    goto error; \
			} else { \
			    DEBUG(1, ("OK   : %s\n", msg)); \
			}

WERROR WBEM_RemoteExecute(struct IWbemServices *pWS, const char *cmdline, uint32_t *ret_code)
{
	struct IWbemClassObject *wco = NULL;
	struct IWbemClassObject *inc, *outc, *in;
	struct IWbemClassObject *out = NULL;
	WERROR result;
	union CIMVAR v;
	TALLOC_CTX *ctx;

	ctx = talloc_new(0);

	result = IWbemServices_GetObject(pWS, ctx, "Win32_Process", WBEM_FLAG_RETURN_WBEM_COMPLETE, NULL, &wco, NULL);
	WERR_CHECK("GetObject.");

	result = IWbemClassObject_GetMethod(wco, ctx, "Create", 0, &inc, &outc);
	WERR_CHECK("IWbemClassObject_GetMethod.");

	result = IWbemClassObject_SpawnInstance(inc, ctx, 0, &in);
	WERR_CHECK("IWbemClassObject_SpawnInstance.");

	v.v_string = cmdline;
	result = IWbemClassObject_Put(in, ctx, "CommandLine", 0, &v, 0);
	WERR_CHECK("IWbemClassObject_Put(CommandLine).");

	result = IWbemServices_ExecMethod(pWS, ctx, "Win32_Process", "Create", 0, NULL, in, &out, NULL);
	WERR_CHECK("IWbemServices_ExecMethod.");

	if (ret_code) {
		result = WbemClassObject_Get(out->object_data, ctx, "ReturnValue", 0, &v, 0, 0);
		WERR_CHECK("IWbemClassObject_Put(CommandLine).");
		*ret_code = v.v_uint32;
	}
error:
	talloc_free(ctx);
	return result;
}

int main(int argc, char **argv)
{
	struct program_args args = {};
	struct com_context *ctx = NULL;
	WERROR result;
	NTSTATUS status;
	struct IWbemServices *pWS = NULL;
        struct IEnumWbemClassObject *pEnum = NULL;
	uint32_t cnt;

        parse_args(argc, argv, &args);

	dcerpc_init();
	dcerpc_table_init();

	dcom_proxy_IUnknown_init();
	dcom_proxy_IWbemLevel1Login_init();
	dcom_proxy_IWbemServices_init();
	dcom_proxy_IEnumWbemClassObject_init();
	dcom_proxy_IRemUnknown_init();
	dcom_proxy_IWbemFetchSmartEnum_init();
	dcom_proxy_IWbemWCOSmartEnum_init();
	dcom_proxy_IWbemClassObject_init();

	com_init_ctx(&ctx, NULL);
	dcom_client_init(ctx, cmdline_credentials);

	result = WBEM_ConnectServer(ctx, args.hostname, "root\\cimv2", 0, 0, 0, 0, 0, 0, &pWS);
	WERR_CHECK("WBEM_ConnectServer.");

	DEBUG(0, ("1: Creating directory C:\\wmi_test_dir_tmp using method Win32_Process.Create\n"));
	WBEM_RemoteExecute(pWS, "cmd.exe /C mkdir C:\\wmi_test_dir_tmp", &cnt);
	WERR_CHECK("WBEM_RemoteExecute.");
	DEBUG(0, ("2: ReturnCode: %d\n", cnt));

	DEBUG(0, ("3: Monitoring directory C:\\wmi_test_dir_tmp. Please create/delete files in that directory to see notifications, after 4 events program quits.\n"));
        result = IWbemServices_ExecNotificationQuery(pWS, ctx, "WQL", "SELECT * FROM __InstanceOperationEvent WITHIN 1 WHERE Targetinstance ISA 'CIM_DirectoryContainsFile' and TargetInstance.GroupComponent= 'Win32_Directory.Name=\"C:\\\\\\\\wmi_test_dir_tmp\"'", WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY, NULL, &pEnum);
        WERR_CHECK("WMI query execute.");
	for (cnt = 0; cnt < 4; ++cnt) {
		struct WbemClassObject *co;
		uint32_t ret;
		result = IEnumWbemClassObject_SmartNext(pEnum, ctx, 0xFFFFFFFF, 1, &co, &ret);
    		WERR_CHECK("IEnumWbemClassObject_Next.");
		printf("%s\n", co->obj_class->__CLASS);
	}

error:
	status = werror_to_ntstatus(result);
	fprintf(stderr, "NTSTATUS: %s - %s\n", nt_errstr(status), get_friendly_nt_error_msg(status));
	talloc_free(ctx);
	return 1;
}
