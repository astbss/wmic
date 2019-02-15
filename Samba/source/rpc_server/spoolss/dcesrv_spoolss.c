/* 
   Unix SMB/CIFS implementation.

   endpoint server for the spoolss pipe

   Copyright (C) Tim Potter 2004
   Copyright (C) Stefan Metzmacher 2005
   
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
#include "rpc_server/dcerpc_server.h"
#include "librpc/gen_ndr/ndr_spoolss.h"
#include "rpc_server/common/common.h"
#include "ntptr/ntptr.h"
#include "lib/socket/socket.h"
#include "smbd/service_stream.h"

#define SPOOLSS_BUFFER_UNION(fn,info,level) \
	((info)?ndr_size_##fn(info, level, 0):0)

#define SPOOLSS_BUFFER_UNION_ARRAY(fn,info,level,count) \
	((info)?ndr_size_##fn##_info(dce_call, level, count, info):0)

#define SPOOLSS_BUFFER_OK(val_true,val_false) ((r->in.offered >= r->out.needed)?val_true:val_false)

static WERROR spoolss_parse_printer_name(TALLOC_CTX *mem_ctx, const char *name,
					 const char **_server_name,
					 const char **_object_name,
					 enum ntptr_HandleType *_object_type)
{
	char *p;
	char *server = NULL;
	char *server_unc = NULL;
	const char *object = name;

	/* no printername is there it's like open server */
	if (!name) {
		*_server_name = NULL;
		*_object_name = NULL;
		*_object_type = NTPTR_HANDLE_SERVER;
		return WERR_OK;
	}

	/* just "\\" is invalid */
	if (strequal("\\\\", name)) {
		return WERR_INVALID_PRINTER_NAME;
	}

	if (strncmp("\\\\", name, 2) == 0) {
		server_unc = talloc_strdup(mem_ctx, name);
		W_ERROR_HAVE_NO_MEMORY(server_unc);
		server = server_unc + 2;

		/* here we know we have "\\" in front not followed
		 * by '\0', now see if we have another "\" in the string
		 */
		p = strchr_m(server, '\\');
		if (!p) {
			/* there's no other "\", so it's ("\\%s",server)
			 */
			*_server_name = server_unc;
			*_object_name = NULL;
			*_object_type = NTPTR_HANDLE_SERVER;
			return WERR_OK;
		}
		/* here we know that we have ("\\%s\",server),
		 * if we have '\0' as next then it's an invalid name
		 * otherwise the printer_name
		 */
		p[0] = '\0';
		/* everything that follows is the printer name */
		p++;
		object = p;

		/* just "" as server is invalid */
		if (strequal(server, "")) {
			return WERR_INVALID_PRINTER_NAME;
		}
	}

	/* just "" is invalid */
	if (strequal(object, "")) {
		return WERR_INVALID_PRINTER_NAME;
	}

#define XCV_PORT ",XcvPort "
#define XCV_MONITOR ",XcvMonitor "
	if (strncmp(object, XCV_PORT, strlen(XCV_PORT)) == 0) {
		object += strlen(XCV_PORT);

		/* just "" is invalid */
		if (strequal(object, "")) {
			return WERR_INVALID_PRINTER_NAME;
		}

		*_server_name = server_unc;
		*_object_name = object;
		*_object_type = NTPTR_HANDLE_PORT;
		return WERR_OK;
	} else if (strncmp(object, XCV_MONITOR, strlen(XCV_MONITOR)) == 0) {
		object += strlen(XCV_MONITOR);

		/* just "" is invalid */
		if (strequal(object, "")) {
			return WERR_INVALID_PRINTER_NAME;
		}

		*_server_name = server_unc;
		*_object_name = object;
		*_object_type = NTPTR_HANDLE_MONITOR;
		return WERR_OK;
	}

	*_server_name = server_unc;
	*_object_name = object;
	*_object_type = NTPTR_HANDLE_PRINTER;
	return WERR_OK;
}

/*
 * Check server_name is:
 * -  "" , functions that don't allow "",
 *         should check that on their own, before calling this function
 * -  our name (only netbios yet, TODO: need to test dns name!)
 * -  our ip address of the current use socket
 * otherwise return WERR_INVALID_PRINTER_NAME
 */
static WERROR spoolss_check_server_name(struct dcesrv_call_state *dce_call, 
					TALLOC_CTX *mem_ctx,
					const char *server_name)
{
	BOOL ret;
	struct socket_address *myaddr;
	const char **aliases;
	int i;

	/* NULL is ok */
	if (!server_name) return WERR_OK;

	/* "" is ok */
	ret = strequal("",server_name);
	if (ret) return WERR_OK;

	/* just "\\" is invalid */
	if (strequal("\\\\", server_name)) {
		return WERR_INVALID_PRINTER_NAME;
	}

	/* then we need "\\" */
	if (strncmp("\\\\", server_name, 2) != 0) {
		return WERR_INVALID_PRINTER_NAME;
	}

	server_name += 2;

	/* NETBIOS NAME is ok */
	ret = strequal(lp_netbios_name(), server_name);
	if (ret) return WERR_OK;

	aliases = lp_netbios_aliases();

	for (i=0; aliases && aliases[i]; i++) {
		if (strequal(aliases[i], server_name)) {
			return WERR_OK;
		}
	}

	/* DNS NAME is ok
	 * TODO: we need to check if aliases are also ok
	 */
	if (lp_realm()) {
		char *str;

		str = talloc_asprintf(mem_ctx, "%s.%s",
						lp_netbios_name(),
						lp_realm());
		W_ERROR_HAVE_NO_MEMORY(str);

		ret = strequal(str, server_name);
		talloc_free(str);
		if (ret) return WERR_OK;
	}

	myaddr = dcesrv_connection_get_my_addr(dce_call->conn, mem_ctx);
	W_ERROR_HAVE_NO_MEMORY(myaddr);

	ret = strequal(myaddr->addr, server_name);
	talloc_free(myaddr);
	if (ret) return WERR_OK;

	return WERR_INVALID_PRINTER_NAME;
}

static NTSTATUS dcerpc_spoolss_bind(struct dcesrv_call_state *dce_call, const struct dcesrv_interface *iface)
{
	NTSTATUS status;
	struct ntptr_context *ntptr;

	status = ntptr_init_context(dce_call->context, lp_ntptr_providor(), &ntptr);
	NT_STATUS_NOT_OK_RETURN(status);

	dce_call->context->private = ntptr;

	return NT_STATUS_OK;
}

#define DCESRV_INTERFACE_SPOOLSS_BIND dcerpc_spoolss_bind

/* 
  spoolss_EnumPrinters 
*/
static WERROR spoolss_EnumPrinters(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_EnumPrinters *r)
{
	struct ntptr_context *ntptr = talloc_get_type(dce_call->context->private, struct ntptr_context);
	WERROR status;

	status = spoolss_check_server_name(dce_call, mem_ctx, r->in.server);
	W_ERROR_NOT_OK_RETURN(status);

	status = ntptr_EnumPrinters(ntptr, mem_ctx, r);
	W_ERROR_NOT_OK_RETURN(status);

	r->out.needed	= SPOOLSS_BUFFER_UNION_ARRAY(spoolss_EnumPrinters, r->out.info, r->in.level, r->out.count);
	r->out.info	= SPOOLSS_BUFFER_OK(r->out.info, NULL);
	r->out.count	= SPOOLSS_BUFFER_OK(r->out.count, 0);
	return SPOOLSS_BUFFER_OK(WERR_OK, WERR_INSUFFICIENT_BUFFER);
}

static WERROR spoolss_OpenPrinterEx(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_OpenPrinterEx *r);
/* 
  spoolss_OpenPrinter 
*/
static WERROR spoolss_OpenPrinter(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_OpenPrinter *r)
{
	WERROR status;
	struct spoolss_OpenPrinterEx *r2;

	r2 = talloc(mem_ctx, struct spoolss_OpenPrinterEx);
	W_ERROR_HAVE_NO_MEMORY(r2);

	r2->in.printername	= r->in.printername;
	r2->in.datatype		= r->in.datatype;
	r2->in.devmode_ctr	= r->in.devmode_ctr;
	r2->in.access_mask	= r->in.access_mask;
	r2->in.level		= 1;
	r2->in.userlevel.level1	= NULL;

	r2->out.handle		= r->out.handle;

	/* TODO: we should take care about async replies here,
	         if spoolss_OpenPrinterEx() would be async!
	 */
	status = spoolss_OpenPrinterEx(dce_call, mem_ctx, r2);

	r->out.handle		= r2->out.handle;

	return status;
}


/* 
  spoolss_SetJob 
*/
static WERROR spoolss_SetJob(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_SetJob *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_GetJob 
*/
static WERROR spoolss_GetJob(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_GetJob *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_EnumJobs 
*/
static WERROR spoolss_EnumJobs(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_EnumJobs *r)
{
	return WERR_OK;
}


/* 
  spoolss_AddPrinter 
*/
static WERROR spoolss_AddPrinter(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_AddPrinter *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_DeletePrinter 
*/
static WERROR spoolss_DeletePrinter(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_DeletePrinter *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_SetPrinter 
*/
static WERROR spoolss_SetPrinter(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_SetPrinter *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_GetPrinter 
*/
static WERROR spoolss_GetPrinter(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_GetPrinter *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_AddPrinterDriver 
*/
static WERROR spoolss_AddPrinterDriver(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_AddPrinterDriver *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_EnumPrinterDrivers 
*/
static WERROR spoolss_EnumPrinterDrivers(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_EnumPrinterDrivers *r)
{
	struct ntptr_context *ntptr = talloc_get_type(dce_call->context->private, struct ntptr_context);
	WERROR status;

	status = spoolss_check_server_name(dce_call, mem_ctx, r->in.server);
	W_ERROR_NOT_OK_RETURN(status);

	status = ntptr_EnumPrinterDrivers(ntptr, mem_ctx, r);
	W_ERROR_NOT_OK_RETURN(status);

	r->out.needed	= SPOOLSS_BUFFER_UNION_ARRAY(spoolss_EnumPrinterDrivers, r->out.info, r->in.level, r->out.count);
	r->out.info	= SPOOLSS_BUFFER_OK(r->out.info, NULL);
	r->out.count	= SPOOLSS_BUFFER_OK(r->out.count, 0);
	return SPOOLSS_BUFFER_OK(WERR_OK, WERR_INSUFFICIENT_BUFFER);
}


/* 
  spoolss_GetPrinterDriver 
*/
static WERROR spoolss_GetPrinterDriver(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_GetPrinterDriver *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_GetPrinterDriverDirectory 
*/
static WERROR spoolss_GetPrinterDriverDirectory(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_GetPrinterDriverDirectory *r)
{
	struct ntptr_context *ntptr = talloc_get_type(dce_call->context->private, struct ntptr_context);
	WERROR status;

	status = spoolss_check_server_name(dce_call, mem_ctx, r->in.server);
	W_ERROR_NOT_OK_RETURN(status);

	status = ntptr_GetPrinterDriverDirectory(ntptr, mem_ctx, r);
	W_ERROR_NOT_OK_RETURN(status);

	r->out.needed	= SPOOLSS_BUFFER_UNION(spoolss_DriverDirectoryInfo, r->out.info, r->in.level);
	r->out.info	= SPOOLSS_BUFFER_OK(r->out.info, NULL);
	return SPOOLSS_BUFFER_OK(WERR_OK, WERR_INSUFFICIENT_BUFFER);
}


/* 
  spoolss_DeletePrinterDriver 
*/
static WERROR spoolss_DeletePrinterDriver(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_DeletePrinterDriver *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_AddPrintProcessor 
*/
static WERROR spoolss_AddPrintProcessor(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_AddPrintProcessor *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_EnumPrintProcessors 
*/
static WERROR spoolss_EnumPrintProcessors(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_EnumPrintProcessors *r)
{
	return WERR_OK;
}


/* 
  spoolss_GetPrintProcessorDirectory 
*/
static WERROR spoolss_GetPrintProcessorDirectory(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_GetPrintProcessorDirectory *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_StartDocPrinter 
*/
static WERROR spoolss_StartDocPrinter(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_StartDocPrinter *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_StartPagePrinter 
*/
static WERROR spoolss_StartPagePrinter(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_StartPagePrinter *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_WritePrinter 
*/
static WERROR spoolss_WritePrinter(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_WritePrinter *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_EndPagePrinter 
*/
static WERROR spoolss_EndPagePrinter(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_EndPagePrinter *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_AbortPrinter 
*/
static WERROR spoolss_AbortPrinter(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_AbortPrinter *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_ReadPrinter 
*/
static WERROR spoolss_ReadPrinter(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_ReadPrinter *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_EndDocPrinter 
*/
static WERROR spoolss_EndDocPrinter(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_EndDocPrinter *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_AddJob 
*/
static WERROR spoolss_AddJob(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_AddJob *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_ScheduleJob 
*/
static WERROR spoolss_ScheduleJob(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_ScheduleJob *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_GetPrinterData 
*/
static WERROR spoolss_GetPrinterData(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_GetPrinterData *r)
{
	struct ntptr_GenericHandle *handle;
	struct dcesrv_handle *h;
	WERROR status;

	DCESRV_PULL_HANDLE_WERR(h, r->in.handle, DCESRV_HANDLE_ANY);
	handle = talloc_get_type(h->data, struct ntptr_GenericHandle);
	if (!handle)
		return WERR_BADFID;

	switch (handle->type) {
		case NTPTR_HANDLE_SERVER:
			status = ntptr_GetPrintServerData(handle, mem_ctx, r);
			break;
		default:
			status = WERR_FOOBAR;
			break;
	}

	W_ERROR_NOT_OK_RETURN(status);

	r->out.needed	= ndr_size_spoolss_PrinterData(&r->out.data, r->out.type, 0);
	r->out.type	= SPOOLSS_BUFFER_OK(r->out.type, SPOOLSS_PRINTER_DATA_TYPE_NULL);
	r->out.data	= SPOOLSS_BUFFER_OK(r->out.data, r->out.data);
	return SPOOLSS_BUFFER_OK(WERR_OK, WERR_MORE_DATA);
}


/* 
  spoolss_SetPrinterData 
*/
static WERROR spoolss_SetPrinterData(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_SetPrinterData *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_WaitForPrinterChange 
*/
static WERROR spoolss_WaitForPrinterChange(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_WaitForPrinterChange *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_ClosePrinter 
*/
static WERROR spoolss_ClosePrinter(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_ClosePrinter *r)
{
	struct dcesrv_handle *h;

	*r->out.handle = *r->in.handle;

	DCESRV_PULL_HANDLE_WERR(h, r->in.handle, DCESRV_HANDLE_ANY);

	talloc_free(h);

	ZERO_STRUCTP(r->out.handle);

	return WERR_OK;
}


/* 
  spoolss_AddForm 
*/
static WERROR spoolss_AddForm(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_AddForm *r)
{
	struct ntptr_GenericHandle *handle;
	struct dcesrv_handle *h;
	WERROR status;

	DCESRV_PULL_HANDLE_WERR(h, r->in.handle, DCESRV_HANDLE_ANY);
	handle = talloc_get_type(h->data, struct ntptr_GenericHandle);
	if (!handle)
		return WERR_BADFID;

	switch (handle->type) {
		case NTPTR_HANDLE_SERVER:
			status = ntptr_AddPrintServerForm(handle, mem_ctx, r);
			W_ERROR_NOT_OK_RETURN(status);
			break;
		case NTPTR_HANDLE_PRINTER:
			status = ntptr_AddPrinterForm(handle, mem_ctx, r);
			W_ERROR_NOT_OK_RETURN(status);
			break;
		default:
			return WERR_FOOBAR;
	}

	return WERR_OK;
}


/* 
  spoolss_DeleteForm 
*/
static WERROR spoolss_DeleteForm(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_DeleteForm *r)
{
	struct ntptr_GenericHandle *handle;
	struct dcesrv_handle *h;
	WERROR status;

	DCESRV_PULL_HANDLE_WERR(h, r->in.handle, DCESRV_HANDLE_ANY);
	handle = talloc_get_type(h->data, struct ntptr_GenericHandle);
	if (!handle)
		return WERR_BADFID;

	switch (handle->type) {
		case NTPTR_HANDLE_SERVER:
			status = ntptr_DeletePrintServerForm(handle, mem_ctx, r);
			W_ERROR_NOT_OK_RETURN(status);
			break;
		case NTPTR_HANDLE_PRINTER:
			status = ntptr_DeletePrinterForm(handle, mem_ctx, r);
			W_ERROR_NOT_OK_RETURN(status);
			break;
		default:
			return WERR_FOOBAR;
	}

	return WERR_OK;
}


/* 
  spoolss_GetForm 
*/
static WERROR spoolss_GetForm(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_GetForm *r)
{
	struct ntptr_GenericHandle *handle;
	struct dcesrv_handle *h;
	WERROR status;

	DCESRV_PULL_HANDLE_WERR(h, r->in.handle, DCESRV_HANDLE_ANY);
	handle = talloc_get_type(h->data, struct ntptr_GenericHandle);
	if (!handle)
		return WERR_BADFID;

	switch (handle->type) {
		case NTPTR_HANDLE_SERVER:
			/*
			 * stupid, but w2k3 returns WERR_BADFID here?
			 */
			return WERR_BADFID;
		case NTPTR_HANDLE_PRINTER:
			status = ntptr_GetPrinterForm(handle, mem_ctx, r);
			W_ERROR_NOT_OK_RETURN(status);
			break;
		default:
			return WERR_FOOBAR;
	}

	r->out.needed	= SPOOLSS_BUFFER_UNION(spoolss_FormInfo, r->out.info, r->in.level);
	r->out.info	= SPOOLSS_BUFFER_OK(r->out.info, NULL);
	return SPOOLSS_BUFFER_OK(WERR_OK, WERR_INSUFFICIENT_BUFFER);
}


/* 
  spoolss_SetForm 
*/
static WERROR spoolss_SetForm(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_SetForm *r)
{
	struct ntptr_GenericHandle *handle;
	struct dcesrv_handle *h;
	WERROR status;

	DCESRV_PULL_HANDLE_WERR(h, r->in.handle, DCESRV_HANDLE_ANY);
	handle = talloc_get_type(h->data, struct ntptr_GenericHandle);
	if (!handle)
		return WERR_BADFID;

	switch (handle->type) {
		case NTPTR_HANDLE_SERVER:
			status = ntptr_SetPrintServerForm(handle, mem_ctx, r);
			W_ERROR_NOT_OK_RETURN(status);
			break;
		case NTPTR_HANDLE_PRINTER:
			status = ntptr_SetPrinterForm(handle, mem_ctx, r);
			W_ERROR_NOT_OK_RETURN(status);
			break;
		default:
			return WERR_FOOBAR;
	}

	return WERR_OK;
}


/* 
  spoolss_EnumForms 
*/
static WERROR spoolss_EnumForms(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_EnumForms *r)
{
	struct ntptr_GenericHandle *handle;
	struct dcesrv_handle *h;
	WERROR status;

	DCESRV_PULL_HANDLE_WERR(h, r->in.handle, DCESRV_HANDLE_ANY);
	handle = talloc_get_type(h->data, struct ntptr_GenericHandle);
	if (!handle)
		return WERR_BADFID;

	switch (handle->type) {
		case NTPTR_HANDLE_SERVER:
			status = ntptr_EnumPrintServerForms(handle, mem_ctx, r);
			W_ERROR_NOT_OK_RETURN(status);
			break;
		case NTPTR_HANDLE_PRINTER:
			status = ntptr_EnumPrinterForms(handle, mem_ctx, r);
			W_ERROR_NOT_OK_RETURN(status);
			break;
		default:
			return WERR_FOOBAR;
	}

	r->out.needed	= SPOOLSS_BUFFER_UNION_ARRAY(spoolss_EnumForms, r->out.info, r->in.level, r->out.count);
	r->out.info	= SPOOLSS_BUFFER_OK(r->out.info, NULL);
	r->out.count	= SPOOLSS_BUFFER_OK(r->out.count, 0);
	return SPOOLSS_BUFFER_OK(WERR_OK, WERR_INSUFFICIENT_BUFFER);
}


/* 
  spoolss_EnumPorts 
*/
static WERROR spoolss_EnumPorts(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_EnumPorts *r)
{
	struct ntptr_context *ntptr = talloc_get_type(dce_call->context->private, struct ntptr_context);
	WERROR status;

	status = spoolss_check_server_name(dce_call, mem_ctx, r->in.servername);
	W_ERROR_NOT_OK_RETURN(status);

	status = ntptr_EnumPorts(ntptr, mem_ctx, r);
	W_ERROR_NOT_OK_RETURN(status);

	r->out.needed	= SPOOLSS_BUFFER_UNION_ARRAY(spoolss_EnumPorts, r->out.info, r->in.level, r->out.count);
	r->out.info	= SPOOLSS_BUFFER_OK(r->out.info, NULL);
	r->out.count	= SPOOLSS_BUFFER_OK(r->out.count, 0);
	return SPOOLSS_BUFFER_OK(WERR_OK, WERR_INSUFFICIENT_BUFFER);
}


/* 
  spoolss_EnumMonitors 
*/
static WERROR spoolss_EnumMonitors(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_EnumMonitors *r)
{
	struct ntptr_context *ntptr = talloc_get_type(dce_call->context->private, struct ntptr_context);
	WERROR status;

	status = spoolss_check_server_name(dce_call, mem_ctx, r->in.servername);
	W_ERROR_NOT_OK_RETURN(status);

	status = ntptr_EnumMonitors(ntptr, mem_ctx, r);
	W_ERROR_NOT_OK_RETURN(status);

	r->out.needed	= SPOOLSS_BUFFER_UNION_ARRAY(spoolss_EnumMonitors, r->out.info, r->in.level, r->out.count);
	r->out.info	= SPOOLSS_BUFFER_OK(r->out.info, NULL);
	r->out.count	= SPOOLSS_BUFFER_OK(r->out.count, 0);
	return SPOOLSS_BUFFER_OK(WERR_OK, WERR_INSUFFICIENT_BUFFER);
}


/* 
  spoolss_AddPort 
*/
static WERROR spoolss_AddPort(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_AddPort *r)
{
	return WERR_NOT_SUPPORTED;
}


/* 
  spoolss_ConfigurePort 
*/
static WERROR spoolss_ConfigurePort(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_ConfigurePort *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_DeletePort 
*/
static WERROR spoolss_DeletePort(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_DeletePort *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_CreatePrinterIC 
*/
static WERROR spoolss_CreatePrinterIC(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_CreatePrinterIC *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_PlayGDIScriptOnPrinterIC 
*/
static WERROR spoolss_PlayGDIScriptOnPrinterIC(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_PlayGDIScriptOnPrinterIC *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_DeletePrinterIC 
*/
static WERROR spoolss_DeletePrinterIC(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_DeletePrinterIC *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_AddPrinterConnection 
*/
static WERROR spoolss_AddPrinterConnection(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_AddPrinterConnection *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_DeletePrinterConnection 
*/
static WERROR spoolss_DeletePrinterConnection(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_DeletePrinterConnection *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_PrinterMessageBox 
*/
static WERROR spoolss_PrinterMessageBox(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_PrinterMessageBox *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_AddMonitor 
*/
static WERROR spoolss_AddMonitor(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_AddMonitor *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_DeleteMonitor 
*/
static WERROR spoolss_DeleteMonitor(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_DeleteMonitor *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_DeletePrintProcessor 
*/
static WERROR spoolss_DeletePrintProcessor(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_DeletePrintProcessor *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_AddPrintProvidor 
*/
static WERROR spoolss_AddPrintProvidor(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_AddPrintProvidor *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_DeletePrintProvidor 
*/
static WERROR spoolss_DeletePrintProvidor(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_DeletePrintProvidor *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_EnumPrintProcDataTypes 
*/
static WERROR spoolss_EnumPrintProcDataTypes(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_EnumPrintProcDataTypes *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_ResetPrinter 
*/
static WERROR spoolss_ResetPrinter(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_ResetPrinter *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_GetPrinterDriver2 
*/
static WERROR spoolss_GetPrinterDriver2(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_GetPrinterDriver2 *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_FindFirstPrinterChangeNotification 
*/
static WERROR spoolss_FindFirstPrinterChangeNotification(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_FindFirstPrinterChangeNotification *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_FindNextPrinterChangeNotification 
*/
static WERROR spoolss_FindNextPrinterChangeNotification(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_FindNextPrinterChangeNotification *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_FindClosePrinterNotify 
*/
static WERROR spoolss_FindClosePrinterNotify(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_FindClosePrinterNotify *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_RouterFindFirstPrinterChangeNotificationOld 
*/
static WERROR spoolss_RouterFindFirstPrinterChangeNotificationOld(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_RouterFindFirstPrinterChangeNotificationOld *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_ReplyOpenPrinter 
*/
static WERROR spoolss_ReplyOpenPrinter(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_ReplyOpenPrinter *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_RouterReplyPrinter 
*/
static WERROR spoolss_RouterReplyPrinter(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_RouterReplyPrinter *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_ReplyClosePrinter 
*/
static WERROR spoolss_ReplyClosePrinter(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_ReplyClosePrinter *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_AddPortEx 
*/
static WERROR spoolss_AddPortEx(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_AddPortEx *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_RouterFindFirstPrinterChangeNotification 
*/
static WERROR spoolss_RouterFindFirstPrinterChangeNotification(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_RouterFindFirstPrinterChangeNotification *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_SpoolerInit 
*/
static WERROR spoolss_SpoolerInit(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_SpoolerInit *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_ResetPrinterEx 
*/
static WERROR spoolss_ResetPrinterEx(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_ResetPrinterEx *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_RemoteFindFirstPrinterChangeNotifyEx 
*/
static WERROR spoolss_RemoteFindFirstPrinterChangeNotifyEx(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_RemoteFindFirstPrinterChangeNotifyEx *r)
{
	/*
	 * TODO: for now just return ok,
	 *       to keep the w2k3 PrintServer 
	 *       happy to allow to open the Add Printer GUI
	 */
	return WERR_OK;
}


/* 
  spoolss_RouterRefreshPrinterChangeNotification 
*/
static WERROR spoolss_RouterRefreshPrinterChangeNotification(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_RouterRefreshPrinterChangeNotification *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_RemoteFindNextPrinterChangeNotifyEx 
*/
static WERROR spoolss_RemoteFindNextPrinterChangeNotifyEx(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_RemoteFindNextPrinterChangeNotifyEx *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_44 
*/
static WERROR spoolss_44(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_44 *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}

/* 
  spoolss_OpenPrinterEx 
*/
static WERROR spoolss_OpenPrinterEx(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_OpenPrinterEx *r)
{
	struct ntptr_context *ntptr = talloc_get_type(dce_call->context->private, struct ntptr_context);
	struct ntptr_GenericHandle *handle;
	struct dcesrv_handle *h;
	const char *server;
	const char *object;
	enum ntptr_HandleType type;
	WERROR status;

	ZERO_STRUCTP(r->out.handle);

	status = spoolss_parse_printer_name(mem_ctx, r->in.printername, &server, &object, &type);
	W_ERROR_NOT_OK_RETURN(status);

	status = spoolss_check_server_name(dce_call, mem_ctx, server);
	W_ERROR_NOT_OK_RETURN(status);

	switch (type) {
		case NTPTR_HANDLE_SERVER:
			status = ntptr_OpenPrintServer(ntptr, mem_ctx, r, server, &handle);
			W_ERROR_NOT_OK_RETURN(status);
			break;
		case NTPTR_HANDLE_PORT:
			status = ntptr_OpenPort(ntptr, mem_ctx, r, object, &handle);
			W_ERROR_NOT_OK_RETURN(status);
			break;
		case NTPTR_HANDLE_MONITOR:
			status = ntptr_OpenMonitor(ntptr, mem_ctx, r, object, &handle);
			W_ERROR_NOT_OK_RETURN(status);
			break;
		case NTPTR_HANDLE_PRINTER:
			status = ntptr_OpenPrinter(ntptr, mem_ctx, r, object, &handle);
			W_ERROR_NOT_OK_RETURN(status);
			break;
		default:
			return WERR_FOOBAR;
	}

	h = dcesrv_handle_new(dce_call->context, handle->type);
	W_ERROR_HAVE_NO_MEMORY(h);

	h->data = talloc_steal(h, handle);

	*r->out.handle	= h->wire_handle;

	return WERR_OK;
}

/* 
  spoolss_AddPrinterEx 
*/
static WERROR spoolss_AddPrinterEx(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_AddPrinterEx *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_47 
*/
static WERROR spoolss_47(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_47 *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_EnumPrinterData 
*/
static WERROR spoolss_EnumPrinterData(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_EnumPrinterData *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_DeletePrinterData 
*/
static WERROR spoolss_DeletePrinterData(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_DeletePrinterData *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_4a 
*/
static WERROR spoolss_4a(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_4a *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_4b 
*/
static WERROR spoolss_4b(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_4b *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_4c 
*/
static WERROR spoolss_4c(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_4c *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_SetPrinterDataEx 
*/
static WERROR spoolss_SetPrinterDataEx(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_SetPrinterDataEx *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_GetPrinterDataEx 
*/
static WERROR spoolss_GetPrinterDataEx(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_GetPrinterDataEx *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_EnumPrinterDataEx 
*/
static WERROR spoolss_EnumPrinterDataEx(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_EnumPrinterDataEx *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_EnumPrinterKey 
*/
static WERROR spoolss_EnumPrinterKey(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_EnumPrinterKey *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_DeletePrinterDataEx 
*/
static WERROR spoolss_DeletePrinterDataEx(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_DeletePrinterDataEx *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_DeletePrinterKey 
*/
static WERROR spoolss_DeletePrinterKey(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_DeletePrinterKey *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_53 
*/
static WERROR spoolss_53(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_53 *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_DeletePrinterDriverEx 
*/
static WERROR spoolss_DeletePrinterDriverEx(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_DeletePrinterDriverEx *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_55 
*/
static WERROR spoolss_55(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_55 *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_56 
*/
static WERROR spoolss_56(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_56 *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_57 
*/
static WERROR spoolss_57(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_57 *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_XcvData
*/
static WERROR spoolss_XcvData(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_XcvData *r)
{
	struct ntptr_GenericHandle *handle;
	struct dcesrv_handle *h;
	WERROR status;

	DCESRV_PULL_HANDLE_WERR(h, r->in.handle, DCESRV_HANDLE_ANY);
	handle = talloc_get_type(h->data, struct ntptr_GenericHandle);

	switch (handle->type) {
		case NTPTR_HANDLE_SERVER:
			status = ntptr_XcvDataPrintServer(handle, mem_ctx, r);
			W_ERROR_NOT_OK_RETURN(status);
			break;
		case NTPTR_HANDLE_PRINTER:
			status = ntptr_XcvDataPrinter(handle, mem_ctx, r);
			W_ERROR_NOT_OK_RETURN(status);
			break;
		case NTPTR_HANDLE_PORT:
			status = ntptr_XcvDataPort(handle, mem_ctx, r);
			W_ERROR_NOT_OK_RETURN(status);
			break;
		case NTPTR_HANDLE_MONITOR:
			status = ntptr_XcvDataMonitor(handle, mem_ctx, r);
			W_ERROR_NOT_OK_RETURN(status);
			break;
		default:
			return WERR_FOOBAR;
	}

	/* TODO: handle the buffer sizes here! */
	return WERR_OK;
}


/* 
  spoolss_AddPrinterDriverEx 
*/
static WERROR spoolss_AddPrinterDriverEx(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_AddPrinterDriverEx *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_5a 
*/
static WERROR spoolss_5a(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_5a *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_5b 
*/
static WERROR spoolss_5b(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_5b *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_5c 
*/
static WERROR spoolss_5c(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_5c *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_5d 
*/
static WERROR spoolss_5d(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_5d *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_5e 
*/
static WERROR spoolss_5e(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_5e *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* 
  spoolss_5f 
*/
static WERROR spoolss_5f(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
		       struct spoolss_5f *r)
{
	DCESRV_FAULT(DCERPC_FAULT_OP_RNG_ERROR);
}


/* include the generated boilerplate */
#include "librpc/gen_ndr/ndr_spoolss_s.c"
