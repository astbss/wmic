/* 
   Unix SMB/CIFS implementation.

   process model manager - structures

   Copyright (C) Andrew Tridgell 1992-2005
   Copyright (C) James J Myers 2003 <myersjj@samba.org>
   Copyright (C) Stefan (metze) Metzmacher 2004-2005
   
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

#ifndef __PROCESS_MODEL_H__
#define __PROCESS_MODEL_H__

#include "lib/socket/socket.h"

/* modules can use the following to determine if the interface has changed
 * please increment the version number after each interface change
 * with a comment and maybe update struct process_model_critical_sizes.
 */
/* version 1 - initial version - metze */
#define PROCESS_MODEL_VERSION 1

/* the process model operations structure - contains function pointers to 
   the model-specific implementations of each operation */
struct model_ops {
	/* the name of the process_model */
	const char *name;

	/* called at startup when the model is selected */
	void (*model_init)(struct event_context *);

	/* function to accept new connection */
	void (*accept_connection)(struct event_context *, struct socket_context *, 
				  void (*)(struct event_context *, struct socket_context *, 
					   uint32_t , void *), 
				  void *);

	/* function to create a task */
	void (*new_task)(struct event_context *, 
			 void (*)(struct event_context *, uint32_t, void *),
			 void *);

	/* function to terminate a connection or task */
	void (*terminate)(struct event_context *, const char *reason);

	/* function to set a title for the connection or task */
	void (*set_title)(struct event_context *, const char *title);
};

/* this structure is used by modules to determine the size of some critical types */
struct process_model_critical_sizes {
	int interface_version;
	int sizeof_model_ops;
};

#include "smbd/process_model_proto.h"

#endif /* __PROCESS_MODEL_H__ */
