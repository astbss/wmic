/* 
   Unix SMB/CIFS implementation.
   seek test suite
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
#include "system/filesys.h"
#include "libcli/raw/libcliraw.h"
#include "libcli/libcli.h"
#include "torture/util.h"

#define CHECK_STATUS(status, correct) do { \
	if (!NT_STATUS_EQUAL(status, correct)) { \
		printf("(%d) Incorrect status %s - should be %s\n", \
		       __LINE__, nt_errstr(status), nt_errstr(correct)); \
		ret = False; \
		goto done; \
	}} while (0)

#define CHECK_VALUE(v, correct) do { \
	if ((v) != (correct)) { \
		printf("(%d) Incorrect value %s=%d - should be %d\n", \
		       __LINE__, #v, (int)v, (int)correct); \
		ret = False; \
		goto done; \
	}} while (0)

#define BASEDIR "\\testseek"

/*
  test seek ops
*/
static BOOL test_seek(struct smbcli_state *cli, TALLOC_CTX *mem_ctx)
{
	union smb_seek io;
	union smb_fileinfo finfo;
	union smb_setfileinfo sfinfo;
	NTSTATUS status;
	BOOL ret = True;
	int fnum, fnum2;
	const char *fname = BASEDIR "\\test.txt";
	uint8_t c[2];

	if (!torture_setup_dir(cli, BASEDIR)) {
		return False;
	}

	fnum = smbcli_open(cli->tree, fname, O_RDWR|O_CREAT|O_TRUNC, DENY_NONE);
	if (fnum == -1) {
		printf("Failed to open test.txt - %s\n", smbcli_errstr(cli->tree));
		ret = False;
		goto done;
	}

	finfo.generic.level = RAW_FILEINFO_POSITION_INFORMATION;
	finfo.position_information.in.file.fnum = fnum;
	
	printf("Trying bad handle\n");
	io.lseek.in.file.fnum = fnum+1;
	io.lseek.in.mode = SEEK_MODE_START;
	io.lseek.in.offset = 0;
	status = smb_raw_seek(cli->tree, &io);
	CHECK_STATUS(status, NT_STATUS_INVALID_HANDLE);

	printf("Trying simple seek\n");
	io.lseek.in.file.fnum = fnum;
	io.lseek.in.mode = SEEK_MODE_START;
	io.lseek.in.offset = 17;
	status = smb_raw_seek(cli->tree, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VALUE(io.lseek.out.offset, 17);
	status = smb_raw_fileinfo(cli->tree, mem_ctx, &finfo);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VALUE(finfo.position_information.out.position, 0);
	
	printf("Trying relative seek\n");
	io.lseek.in.file.fnum = fnum;
	io.lseek.in.mode = SEEK_MODE_CURRENT;
	io.lseek.in.offset = -3;
	status = smb_raw_seek(cli->tree, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VALUE(io.lseek.out.offset, 14);

	printf("Trying end seek\n");
	io.lseek.in.file.fnum = fnum;
	io.lseek.in.mode = SEEK_MODE_END;
	io.lseek.in.offset = 0;
	status = smb_raw_seek(cli->tree, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	finfo.generic.level = RAW_FILEINFO_ALL_INFO;
	finfo.all_info.in.file.fnum = fnum;
	status = smb_raw_fileinfo(cli->tree, mem_ctx, &finfo);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VALUE(io.lseek.out.offset, finfo.all_info.out.size);

	printf("Trying max seek\n");
	io.lseek.in.file.fnum = fnum;
	io.lseek.in.mode = SEEK_MODE_START;
	io.lseek.in.offset = -1;
	status = smb_raw_seek(cli->tree, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VALUE(io.lseek.out.offset, 0xffffffff);

	printf("Testing position information change\n");
	finfo.generic.level = RAW_FILEINFO_POSITION_INFORMATION;
	finfo.position_information.in.file.fnum = fnum;
	status = smb_raw_fileinfo(cli->tree, mem_ctx, &finfo);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VALUE(finfo.position_information.out.position, 0);

	printf("Trying max overflow\n");
	io.lseek.in.file.fnum = fnum;
	io.lseek.in.mode = SEEK_MODE_CURRENT;
	io.lseek.in.offset = 1000;
	status = smb_raw_seek(cli->tree, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VALUE(io.lseek.out.offset, 999);

	printf("Testing position information change\n");
	finfo.generic.level = RAW_FILEINFO_POSITION_INFORMATION;
	finfo.position_information.in.file.fnum = fnum;
	status = smb_raw_fileinfo(cli->tree, mem_ctx, &finfo);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VALUE(finfo.position_information.out.position, 0);

	printf("trying write to update offset\n");
	ZERO_STRUCT(c);
	if (smbcli_write(cli->tree, fnum, 0, c, 0, 2) != 2) {
		printf("Write failed - %s\n", smbcli_errstr(cli->tree));
		ret = False;
		goto done;		
	}

	printf("Testing position information change\n");
	finfo.generic.level = RAW_FILEINFO_POSITION_INFORMATION;
	finfo.position_information.in.file.fnum = fnum;
	status = smb_raw_fileinfo(cli->tree, mem_ctx, &finfo);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VALUE(finfo.position_information.out.position, 0);

	io.lseek.in.file.fnum = fnum;
	io.lseek.in.mode = SEEK_MODE_CURRENT;
	io.lseek.in.offset = 0;
	status = smb_raw_seek(cli->tree, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VALUE(io.lseek.out.offset, 2);
	
	if (smbcli_read(cli->tree, fnum, c, 0, 1) != 1) {
		printf("Read failed - %s\n", smbcli_errstr(cli->tree));
		ret = False;
		goto done;		
	}

	printf("Testing position information change\n");
	finfo.generic.level = RAW_FILEINFO_POSITION_INFORMATION;
	finfo.position_information.in.file.fnum = fnum;
	status = smb_raw_fileinfo(cli->tree, mem_ctx, &finfo);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VALUE(finfo.position_information.out.position, 1);

	status = smb_raw_seek(cli->tree, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VALUE(io.lseek.out.offset, 1);

	printf("Testing position information\n");
	fnum2 = smbcli_open(cli->tree, fname, O_RDWR, DENY_NONE);
	if (fnum2 == -1) {
		printf("2nd open failed - %s\n", smbcli_errstr(cli->tree));
		ret = False;
		goto done;
	}
	sfinfo.generic.level = RAW_SFILEINFO_POSITION_INFORMATION;
	sfinfo.position_information.in.file.fnum = fnum2;
	sfinfo.position_information.in.position = 25;
	status = smb_raw_setfileinfo(cli->tree, &sfinfo);
	CHECK_STATUS(status, NT_STATUS_OK);

	finfo.generic.level = RAW_FILEINFO_POSITION_INFORMATION;
	finfo.position_information.in.file.fnum = fnum2;
	status = smb_raw_fileinfo(cli->tree, mem_ctx, &finfo);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VALUE(finfo.position_information.out.position, 25);

	finfo.generic.level = RAW_FILEINFO_POSITION_INFORMATION;
	finfo.position_information.in.file.fnum = fnum;
	status = smb_raw_fileinfo(cli->tree, mem_ctx, &finfo);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VALUE(finfo.position_information.out.position, 1);

	printf("position_information via paths\n");

	sfinfo.generic.level = RAW_SFILEINFO_POSITION_INFORMATION;
	sfinfo.position_information.in.file.path = fname;
	sfinfo.position_information.in.position = 32;
	status = smb_raw_setpathinfo(cli->tree, &sfinfo);
	CHECK_STATUS(status, NT_STATUS_OK);

	finfo.generic.level = RAW_FILEINFO_POSITION_INFORMATION;
	finfo.position_information.in.file.fnum = fnum2;
	status = smb_raw_fileinfo(cli->tree, mem_ctx, &finfo);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VALUE(finfo.position_information.out.position, 25);

	finfo.generic.level = RAW_FILEINFO_POSITION_INFORMATION;
	finfo.position_information.in.file.path = fname;
	status = smb_raw_pathinfo(cli->tree, mem_ctx, &finfo);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VALUE(finfo.position_information.out.position, 0);
	

done:
	smb_raw_exit(cli->session);
	smbcli_deltree(cli->tree, BASEDIR);
	return ret;
}


/* 
   basic testing of seek calls
*/
BOOL torture_raw_seek(struct torture_context *torture)
{
	struct smbcli_state *cli;
	BOOL ret = True;
	TALLOC_CTX *mem_ctx;

	if (!torture_open_connection(&cli, 0)) {
		return False;
	}

	mem_ctx = talloc_init("torture_raw_seek");

	if (!test_seek(cli, mem_ctx)) {
		ret = False;
	}

	torture_close_connection(cli);
	talloc_free(mem_ctx);
	return ret;
}
