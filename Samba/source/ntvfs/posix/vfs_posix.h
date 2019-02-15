/* 
   Unix SMB/CIFS implementation.

   POSIX NTVFS backend - structure definitions

   Copyright (C) Andrew Tridgell 2004

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

#ifndef _VFS_POSIX_H_
#define _VFS_POSIX_H_

#include "librpc/gen_ndr/xattr.h"
#include "system/filesys.h"
#include "ntvfs/ntvfs.h"
#include "ntvfs/common/proto.h"
#include "dsdb/samdb/samdb.h"

/* this is the private structure for the posix vfs backend. It is used
   to hold per-connection (per tree connect) state information */
struct pvfs_state {
	struct ntvfs_module_context *ntvfs;
	const char *base_directory;
	struct GUID *base_fs_uuid;

	const char *share_name;
	uint_t flags;

	struct pvfs_mangle_context *mangle_ctx;

	struct brl_context *brl_context;
	struct odb_context *odb_context;
	struct notify_context *notify_context;
	struct sidmap_context *sidmap;

	/* a list of pending async requests. Needed to support
	   ntcancel */
	struct pvfs_wait *wait_list;

	/* the sharing violation timeout */
	uint_t sharing_violation_delay;

	/* filesystem attributes (see FS_ATTR_*) */
	uint32_t fs_attribs;

	/* if posix:eadb is set, then this gets setup */
	struct tdb_wrap *ea_db;

	/* the allocation size rounding */
	uint32_t alloc_size_rounding;

	struct {
		/* the open files as DLINKLIST */
		struct pvfs_file *list;
	} files;

	struct {
		/* an id tree mapping open search ID to a pvfs_search_state structure */
		struct idr_context *idtree;

		/* the open searches as DLINKLIST */
		struct pvfs_search_state *list;

		/* how long to keep inactive searches around for */
		uint_t inactivity_time;
	} search;

	/* used to accelerate acl mapping */
	struct {
		const struct dom_sid *creator_owner;
		const struct dom_sid *creator_group;		
	} sid_cache;

	/* the acl backend */
	const struct pvfs_acl_ops *acl_ops;
};

/* this is the basic information needed about a file from the filesystem */
struct pvfs_dos_fileinfo {
	NTTIME create_time;
	NTTIME access_time;
	NTTIME write_time;
	NTTIME change_time;
	uint32_t attrib;
	uint64_t alloc_size;
	uint32_t nlink;
	uint32_t ea_size;
	uint64_t file_id;
	uint32_t flags;
};

/*
  this is the structure returned by pvfs_resolve_name(). It holds the posix details of
  a filename passed by the client to any function
*/
struct pvfs_filename {
	const char *original_name;
	char *full_name;
	const char *stream_name; /* does not include :$DATA suffix */
	uint32_t stream_id;      /* this uses a hash, so is probabilistic */
	BOOL has_wildcard;
	BOOL exists;          /* true if the base filename exists */
	BOOL stream_exists;   /* true if the stream exists */
	struct stat st;
	struct pvfs_dos_fileinfo dos;
};


/* open file handle state - encapsulates the posix fd

   Note that this is separated from the pvfs_file structure in order
   to cope with the openx DENY_DOS semantics where a 2nd DENY_DOS open
   on the same connection gets the same low level filesystem handle,
   rather than a new handle
*/
struct pvfs_file_handle {
	int fd;

	struct pvfs_filename *name;

	/* a unique file key to be used for open file locking */
	DATA_BLOB odb_locking_key;

	uint32_t create_options;

	/* this is set by the mode_information level. What does it do? */
	uint32_t mode;

	/* yes, we need 2 independent positions ... */
	uint64_t seek_offset;
	uint64_t position;

	BOOL have_opendb_entry;

	/* we need this hook back to our parent for lock destruction */
	struct pvfs_state *pvfs;

	/* have we set a sticky write time that we should remove on close */
	BOOL sticky_write_time;
};

/* open file state */
struct pvfs_file {
	struct pvfs_file *next, *prev;
	struct pvfs_file_handle *handle;
	struct ntvfs_handle *ntvfs;

	struct pvfs_state *pvfs;

	uint32_t impersonation;
	uint32_t share_access;
	uint32_t access_mask;

	/* a list of pending locks - used for locking cancel operations */
	struct pvfs_pending_lock *pending_list;

	/* a file handle to be used for byte range locking */
	struct brl_handle *brl_handle;

	/* a count of active locks - used to avoid calling brl_close on
	   file close */
	uint64_t lock_count;

	/* for directories, a buffer of pending notify events */
	struct pvfs_notify_buffer *notify_buffer;

	/* for directories, the state of an incomplete SMB2 Find */
	struct pvfs_search_state *search;
};

/* the state of a search started with pvfs_search_first() */
struct pvfs_search_state {
	struct pvfs_search_state *prev, *next;
	struct pvfs_state *pvfs;
	uint16_t handle;
	off_t current_index;
	uint16_t search_attrib;
	uint16_t must_attrib;
	struct pvfs_dir *dir;
	time_t last_used;
	uint_t num_ea_names;
	struct ea_name *ea_names;
	struct timed_event *te;
};

/* flags to pvfs_resolve_name() */
#define PVFS_RESOLVE_WILDCARD    (1<<0)
#define PVFS_RESOLVE_STREAMS     (1<<1)

/* flags in pvfs->flags */
#define PVFS_FLAG_CI_FILESYSTEM  (1<<0) /* the filesystem is case insensitive */
#define PVFS_FLAG_MAP_ARCHIVE    (1<<1)
#define PVFS_FLAG_MAP_SYSTEM     (1<<2)
#define PVFS_FLAG_MAP_HIDDEN     (1<<3)
#define PVFS_FLAG_READONLY       (1<<4)
#define PVFS_FLAG_STRICT_SYNC    (1<<5)
#define PVFS_FLAG_STRICT_LOCKING (1<<6)
#define PVFS_FLAG_XATTR_ENABLE   (1<<7)
#define PVFS_FLAG_FAKE_OPLOCKS   (1<<8)

/* forward declare some anonymous structures */
struct pvfs_dir;

/* types of notification for pvfs wait events */
enum pvfs_wait_notice {PVFS_WAIT_EVENT, PVFS_WAIT_TIMEOUT, PVFS_WAIT_CANCEL};

#define PVFS_EADB			"posix:eadb"
#define PVFS_XATTR			"posix:xattr"
#define PVFS_FAKE_OPLOCKS		"posix:fakeoplocks"
#define PVFS_SHARE_DELAY		"posix:sharedelay"
#define PVFS_ALLOCATION_ROUNDING	"posix:allocationrounding"
#define PVFS_SEARCH_INACTIVITY		"posix:searchinactivity"
#define PVFS_ACL			"posix:acl"

#define PVFS_XATTR_DEFAULT			True
#define PVFS_FAKE_OPLOCKS_DEFAULT		False
#define PVFS_SHARE_DELAY_DEFAULT		1000000
#define PVFS_ALLOCATION_ROUNDING_DEFAULT	512
#define PVFS_SEARCH_INACTIVITY_DEFAULT		300

struct pvfs_acl_ops {
	const char *name;
	NTSTATUS (*acl_load)(struct pvfs_state *, struct pvfs_filename *, int , TALLOC_CTX *, 
			     struct security_descriptor **);
	NTSTATUS (*acl_save)(struct pvfs_state *, struct pvfs_filename *, int , struct security_descriptor *);
};

#include "ntvfs/posix/vfs_posix_proto.h"

#endif /* _VFS_POSIX_H_ */
