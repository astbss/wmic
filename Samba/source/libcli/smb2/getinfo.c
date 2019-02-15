/* 
   Unix SMB/CIFS implementation.

   SMB2 client getinfo calls

   Copyright (C) Andrew Tridgell 2005
   
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
#include "libcli/raw/libcliraw.h"
#include "libcli/smb2/smb2.h"
#include "libcli/smb2/smb2_calls.h"

/*
  send a getinfo request
*/
struct smb2_request *smb2_getinfo_send(struct smb2_tree *tree, struct smb2_getinfo *io)
{
	struct smb2_request *req;

	req = smb2_request_init_tree(tree, SMB2_OP_GETINFO, 0x28, False, 0);
	if (req == NULL) return NULL;

	/* this seems to be a bug, they use 0x29 but only send 0x28 bytes */
	SSVAL(req->out.body, 0x00, 0x29);

	SSVAL(req->out.body, 0x02, io->in.level);
	SIVAL(req->out.body, 0x04, io->in.max_response_size);
	SIVAL(req->out.body, 0x08, io->in.unknown1);
	SIVAL(req->out.body, 0x0C, io->in.unknown2);
	SIVAL(req->out.body, 0x10, io->in.flags);
	SIVAL(req->out.body, 0x14, io->in.flags2);
	smb2_push_handle(req->out.body+0x18, &io->in.file.handle);

	smb2_transport_send(req);

	return req;
}


/*
  recv a getinfo reply
*/
NTSTATUS smb2_getinfo_recv(struct smb2_request *req, TALLOC_CTX *mem_ctx,
			   struct smb2_getinfo *io)
{
	NTSTATUS status;

	if (!smb2_request_receive(req) || 
	    smb2_request_is_error(req)) {
		return smb2_request_destroy(req);
	}

	SMB2_CHECK_PACKET_RECV(req, 0x08, True);

	status = smb2_pull_o16s16_blob(&req->in, mem_ctx, req->in.body+0x02, &io->out.blob);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	return smb2_request_destroy(req);
}

/*
  sync getinfo request
*/
NTSTATUS smb2_getinfo(struct smb2_tree *tree, TALLOC_CTX *mem_ctx,
		      struct smb2_getinfo *io)
{
	struct smb2_request *req = smb2_getinfo_send(tree, io);
	return smb2_getinfo_recv(req, mem_ctx, io);
}


/*
  map a generic info level to a SMB2 info level
*/
uint16_t smb2_getinfo_map_level(uint16_t level, uint8_t class)
{
	if (class == SMB2_GETINFO_FILE && 
	    level == RAW_FILEINFO_SEC_DESC) {
		return SMB2_GETINFO_SECURITY;
	}
	if ((level & 0xFF) == class) {
		return level;
	} else if (level > 1000) {
		return ((level-1000)<<8) | class;
	}
	DEBUG(0,("Unable to map SMB2 info level 0x%04x of class %d\n", level, class));
	return 0;	
}

/*
  level specific getinfo call - async send
*/
struct smb2_request *smb2_getinfo_file_send(struct smb2_tree *tree, union smb_fileinfo *io)
{
	struct smb2_getinfo b;
	uint16_t smb2_level = smb2_getinfo_map_level(io->generic.level, SMB2_GETINFO_FILE);
	
	if (smb2_level == 0) {
		return NULL;
	}

	ZERO_STRUCT(b);
	b.in.max_response_size = 0x10000;
	b.in.file.handle       = io->generic.in.file.handle;
	b.in.level             = smb2_level;

	if (io->generic.level == RAW_FILEINFO_SEC_DESC) {
		b.in.flags = io->query_secdesc.in.secinfo_flags;
	}
	if (io->generic.level == RAW_FILEINFO_SMB2_ALL_EAS) {
		b.in.flags2 = io->all_eas.in.continue_flags;
	}

	return smb2_getinfo_send(tree, &b);
}

/*
  recv a getinfo reply and parse the level info
*/
NTSTATUS smb2_getinfo_file_recv(struct smb2_request *req, TALLOC_CTX *mem_ctx,
				union smb_fileinfo *io)
{
	struct smb2_getinfo b;
	NTSTATUS status;

	status = smb2_getinfo_recv(req, mem_ctx, &b);
	NT_STATUS_NOT_OK_RETURN(status);

	status = smb_raw_fileinfo_passthru_parse(&b.out.blob, mem_ctx, io->generic.level, io);
	data_blob_free(&b.out.blob);

	return status;
}

/*
  level specific getinfo call
*/
NTSTATUS smb2_getinfo_file(struct smb2_tree *tree, TALLOC_CTX *mem_ctx, 
			   union smb_fileinfo *io)
{
	struct smb2_request *req = smb2_getinfo_file_send(tree, io);
	return smb2_getinfo_file_recv(req, mem_ctx, io);
}


/*
  level specific getinfo call - async send
*/
struct smb2_request *smb2_getinfo_fs_send(struct smb2_tree *tree, union smb_fsinfo *io)
{
	struct smb2_getinfo b;
	uint16_t smb2_level = smb2_getinfo_map_level(io->generic.level, SMB2_GETINFO_FS);
	
	if (smb2_level == 0) {
		return NULL;
	}
	
	ZERO_STRUCT(b);
	b.in.max_response_size = 0x10000;
	b.in.file.handle       = io->generic.handle;
	b.in.level             = smb2_level;

	return smb2_getinfo_send(tree, &b);
}

/*
  recv a getinfo reply and parse the level info
*/
NTSTATUS smb2_getinfo_fs_recv(struct smb2_request *req, TALLOC_CTX *mem_ctx,
				union smb_fsinfo *io)
{
	struct smb2_getinfo b;
	NTSTATUS status;

	status = smb2_getinfo_recv(req, mem_ctx, &b);
	NT_STATUS_NOT_OK_RETURN(status);

	status = smb_raw_fsinfo_passthru_parse(b.out.blob, mem_ctx, io->generic.level, io);
	data_blob_free(&b.out.blob);

	return status;
}

/*
  level specific getinfo call
*/
NTSTATUS smb2_getinfo_fs(struct smb2_tree *tree, TALLOC_CTX *mem_ctx, 
			   union smb_fsinfo *io)
{
	struct smb2_request *req = smb2_getinfo_fs_send(tree, io);
	return smb2_getinfo_fs_recv(req, mem_ctx, io);
}

