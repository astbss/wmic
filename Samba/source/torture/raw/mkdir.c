/* 
   Unix SMB/CIFS implementation.
   RAW_MKDIR_* and RAW_RMDIR_* individual test suite
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
#include "libcli/raw/libcliraw.h"
#include "libcli/libcli.h"
#include "torture/util.h"

#define BASEDIR "\\mkdirtest"

#define CHECK_STATUS(status, correct) do { \
	if (!NT_STATUS_EQUAL(status, correct)) { \
		printf("(%s) Incorrect status %s - should be %s\n", \
		       __location__, nt_errstr(status), nt_errstr(correct)); \
		ret = False; \
		goto done; \
	}} while (0)

/*
  test mkdir ops
*/
static BOOL test_mkdir(struct smbcli_state *cli, TALLOC_CTX *mem_ctx)
{
	union smb_mkdir md;
	struct smb_rmdir rd;
	const char *path = BASEDIR "\\mkdir.dir";
	NTSTATUS status;
	BOOL ret = True;

	if (!torture_setup_dir(cli, BASEDIR)) {
		return False;
	}

	/* 
	   basic mkdir
	*/
	md.mkdir.level = RAW_MKDIR_MKDIR;
	md.mkdir.in.path = path;

	status = smb_raw_mkdir(cli->tree, &md);
	CHECK_STATUS(status, NT_STATUS_OK);

	printf("testing mkdir collision\n");

	/* 2nd create */
	status = smb_raw_mkdir(cli->tree, &md);
	CHECK_STATUS(status, NT_STATUS_OBJECT_NAME_COLLISION);

	/* basic rmdir */
	rd.in.path = path;
	status = smb_raw_rmdir(cli->tree, &rd);
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb_raw_rmdir(cli->tree, &rd);
	CHECK_STATUS(status, NT_STATUS_OBJECT_NAME_NOT_FOUND);

	printf("testing mkdir collision with file\n");

	/* name collision with a file */
	smbcli_close(cli->tree, create_complex_file(cli, mem_ctx, path));
	status = smb_raw_mkdir(cli->tree, &md);
	CHECK_STATUS(status, NT_STATUS_OBJECT_NAME_COLLISION);

	printf("testing rmdir with file\n");

	/* delete a file with rmdir */
	status = smb_raw_rmdir(cli->tree, &rd);
	CHECK_STATUS(status, NT_STATUS_NOT_A_DIRECTORY);

	smbcli_unlink(cli->tree, path);

	printf("testing invalid dir\n");

	/* create an invalid dir */
	md.mkdir.in.path = "..\\..\\..";
	status = smb_raw_mkdir(cli->tree, &md);
	CHECK_STATUS(status, NT_STATUS_OBJECT_PATH_SYNTAX_BAD);
	
	printf("testing t2mkdir\n");

	/* try a t2mkdir - need to work out why this fails! */
	md.t2mkdir.level = RAW_MKDIR_T2MKDIR;
	md.t2mkdir.in.path = path;
	md.t2mkdir.in.num_eas = 0;	
	status = smb_raw_mkdir(cli->tree, &md);
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb_raw_rmdir(cli->tree, &rd);
	CHECK_STATUS(status, NT_STATUS_OK);

	printf("testing t2mkdir with EAs\n");

	/* with EAs */
	md.t2mkdir.level = RAW_MKDIR_T2MKDIR;
	md.t2mkdir.in.path = path;
	md.t2mkdir.in.num_eas = 3;
	md.t2mkdir.in.eas = talloc_array(mem_ctx, struct ea_struct, md.t2mkdir.in.num_eas);
	md.t2mkdir.in.eas[0].flags = 0;
	md.t2mkdir.in.eas[0].name.s = "EAONE";
	md.t2mkdir.in.eas[0].value = data_blob_talloc(mem_ctx, "blah", 4);
	md.t2mkdir.in.eas[1].flags = 0;
	md.t2mkdir.in.eas[1].name.s = "EA TWO";
	md.t2mkdir.in.eas[1].value = data_blob_talloc(mem_ctx, "foo bar", 7);
	md.t2mkdir.in.eas[2].flags = 0;
	md.t2mkdir.in.eas[2].name.s = "EATHREE";
	md.t2mkdir.in.eas[2].value = data_blob_talloc(mem_ctx, "xx1", 3);
	status = smb_raw_mkdir(cli->tree, &md);
	CHECK_STATUS(status, NT_STATUS_OK);

	status = torture_check_ea(cli, path, "EAONE", "blah");
	CHECK_STATUS(status, NT_STATUS_OK);
	status = torture_check_ea(cli, path, "EA TWO", "foo bar");
	CHECK_STATUS(status, NT_STATUS_OK);
	status = torture_check_ea(cli, path, "EATHREE", "xx1");
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb_raw_rmdir(cli->tree, &rd);
	CHECK_STATUS(status, NT_STATUS_OK);

done:
	smb_raw_exit(cli->session);
	smbcli_deltree(cli->tree, BASEDIR);
	return ret;
}


/* 
   basic testing of all RAW_MKDIR_* calls 
*/
BOOL torture_raw_mkdir(struct torture_context *torture)
{
	struct smbcli_state *cli;
	BOOL ret = True;
	TALLOC_CTX *mem_ctx;

	if (!torture_open_connection(&cli, 0)) {
		return False;
	}

	mem_ctx = talloc_init("torture_raw_mkdir");

	if (!test_mkdir(cli, mem_ctx)) {
		ret = False;
	}

	torture_close_connection(cli);
	talloc_free(mem_ctx);
	return ret;
}
