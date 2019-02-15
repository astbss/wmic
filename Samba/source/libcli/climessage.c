/* 
   Unix SMB/CIFS implementation.
   client message handling routines
   Copyright (C) Andrew Tridgell 1994-1998
   Copyright (C) James J Myers 2003  <myersjj@samba.org>
   
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


/****************************************************************************
start a message sequence
****************************************************************************/
BOOL smbcli_message_start(struct smbcli_tree *tree, const char *host, const char *username, 
		       int *grp)
{
	struct smbcli_request *req; 
	
	req = smbcli_request_setup(tree, SMBsendstrt, 0, 0);
	smbcli_req_append_string(req, username, STR_TERMINATE);
	smbcli_req_append_string(req, host, STR_TERMINATE);
	if (!smbcli_request_send(req) || 
	    !smbcli_request_receive(req) ||
	    smbcli_is_error(tree)) {
		smbcli_request_destroy(req);
		return False;
	}

	*grp = SVAL(req->in.vwv, VWV(0));
	smbcli_request_destroy(req);

	return True;
}


/****************************************************************************
send a message 
****************************************************************************/
BOOL smbcli_message_text(struct smbcli_tree *tree, char *msg, int len, int grp)
{
	struct smbcli_request *req; 
	
	req = smbcli_request_setup(tree, SMBsendtxt, 1, 0);
	SSVAL(req->out.vwv, VWV(0), grp);

	smbcli_req_append_bytes(req, (const uint8_t *)msg, len);

	if (!smbcli_request_send(req) || 
	    !smbcli_request_receive(req) ||
	    smbcli_is_error(tree)) {
		smbcli_request_destroy(req);
		return False;
	}

	smbcli_request_destroy(req);
	return True;
}      

/****************************************************************************
end a message 
****************************************************************************/
BOOL smbcli_message_end(struct smbcli_tree *tree, int grp)
{
	struct smbcli_request *req; 
	
	req = smbcli_request_setup(tree, SMBsendend, 1, 0);
	SSVAL(req->out.vwv, VWV(0), grp);

	if (!smbcli_request_send(req) || 
	    !smbcli_request_receive(req) ||
	    smbcli_is_error(tree)) {
		smbcli_request_destroy(req);
		return False;
	}

	smbcli_request_destroy(req);
	return True;
}      

