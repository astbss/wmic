/* 
   Unix SMB/CIFS implementation.

   SMB2 notify test suite

   Copyright (C) Stefan Metzmacher 2006
   
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
#include "libcli/smb2/smb2.h"
#include "libcli/smb2/smb2_calls.h"

#include "torture/torture.h"
#include "torture/smb2/proto.h"

#define CHECK_STATUS(status, correct) do { \
	if (!NT_STATUS_EQUAL(status, correct)) { \
		printf("(%s) Incorrect status %s - should be %s\n", \
		       __location__, nt_errstr(status), nt_errstr(correct)); \
		ret = False; \
		goto done; \
	}} while (0)

#define CHECK_VALUE(v, correct) do { \
	if ((v) != (correct)) { \
		printf("(%s) Incorrect value %s=%d - should be %d\n", \
		       __location__, #v, v, correct); \
		ret = False; \
		goto done; \
	}} while (0)

#define CHECK_WIRE_STR(field, value) do { \
	if (!field.s || strcmp(field.s, value)) { \
		printf("(%s) %s [%s] != %s\n", \
			  __location__, #field, field.s, value); \
		ret = False; \
		goto done; \
	}} while (0)

#define FNAME "smb2-notify01.dat"

static BOOL test_valid_request(TALLOC_CTX *mem_ctx, struct smb2_tree *tree)
{
	BOOL ret = True;
	NTSTATUS status;
	struct smb2_handle dh;
	struct smb2_notify n;
	struct smb2_request *req;

	status = smb2_util_roothandle(tree, &dh);
	CHECK_STATUS(status, NT_STATUS_OK);

	n.in.recursive		= 0x0000;
	n.in.buffer_size	= 0x00080000;
	n.in.file.handle	= dh;
	n.in.completion_filter	= 0x00000FFF;
	n.in.unknown		= 0x00000000;
	req = smb2_notify_send(tree, &n);

	status = torture_setup_complex_file(tree, FNAME);
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb2_notify_recv(req, mem_ctx, &n);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VALUE(n.out.num_changes, 1);
	CHECK_VALUE(n.out.changes[0].action, NOTIFY_ACTION_REMOVED);
	CHECK_WIRE_STR(n.out.changes[0].name, FNAME);

	/* 
	 * if the change response doesn't fit in the buffer
	 * NOTIFY_ENUM_DIR is returned.
	 */
	n.in.buffer_size	= 0x00000000;
	req = smb2_notify_send(tree, &n);

	status = torture_setup_complex_file(tree, FNAME);
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb2_notify_recv(req, mem_ctx, &n);
	CHECK_STATUS(status, STATUS_NOTIFY_ENUM_DIR);

	/* 
	 * if the change response fits in the buffer we get
	 * NT_STATUS_OK again
	 */
	n.in.buffer_size	= 0x00080000;
	req = smb2_notify_send(tree, &n);

	status = torture_setup_complex_file(tree, FNAME);
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb2_notify_recv(req, mem_ctx, &n);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VALUE(n.out.num_changes, 3);
	CHECK_VALUE(n.out.changes[0].action, NOTIFY_ACTION_REMOVED);
	CHECK_WIRE_STR(n.out.changes[0].name, FNAME);
	CHECK_VALUE(n.out.changes[1].action, NOTIFY_ACTION_ADDED);
	CHECK_WIRE_STR(n.out.changes[1].name, FNAME);
	CHECK_VALUE(n.out.changes[2].action, NOTIFY_ACTION_MODIFIED);
	CHECK_WIRE_STR(n.out.changes[2].name, FNAME);

	/* if the first notify returns NOTIFY_ENUM_DIR, all do */
	status = smb2_util_close(tree, dh);
	CHECK_STATUS(status, NT_STATUS_OK);
	status = smb2_util_roothandle(tree, &dh);
	CHECK_STATUS(status, NT_STATUS_OK);

	n.in.recursive		= 0x0000;
	n.in.buffer_size	= 0x00000001;
	n.in.file.handle	= dh;
	n.in.completion_filter	= 0x00000FFF;
	n.in.unknown		= 0x00000000;
	req = smb2_notify_send(tree, &n);

	status = torture_setup_complex_file(tree, FNAME);
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb2_notify_recv(req, mem_ctx, &n);
	CHECK_STATUS(status, STATUS_NOTIFY_ENUM_DIR);

	n.in.buffer_size	= 0x00080000;
	req = smb2_notify_send(tree, &n);

	status = torture_setup_complex_file(tree, FNAME);
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb2_notify_recv(req, mem_ctx, &n);
	CHECK_STATUS(status, STATUS_NOTIFY_ENUM_DIR);

	/* if the buffer size is too large, we get invalid parameter */
	n.in.recursive		= 0x0000;
	n.in.buffer_size	= 0x00080001;
	n.in.file.handle	= dh;
	n.in.completion_filter	= 0x00000FFF;
	n.in.unknown		= 0x00000000;
	req = smb2_notify_send(tree, &n);
	status = smb2_notify_recv(req, mem_ctx, &n);
	CHECK_STATUS(status, NT_STATUS_INVALID_PARAMETER);

done:
	return ret;
}

/* basic testing of SMB2 notify
*/
BOOL torture_smb2_notify(struct torture_context *torture)
{
	TALLOC_CTX *mem_ctx = talloc_new(NULL);
	struct smb2_tree *tree;
	BOOL ret = True;

	if (!torture_smb2_connection(mem_ctx, &tree)) {
		return False;
	}

	ret &= test_valid_request(mem_ctx, tree);

	talloc_free(mem_ctx);

	return ret;
}
