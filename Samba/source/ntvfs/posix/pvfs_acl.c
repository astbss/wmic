/* 
   Unix SMB/CIFS implementation.

   POSIX NTVFS backend - ACL support

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

#include "includes.h"
#include "auth/auth.h"
#include "vfs_posix.h"
#include "librpc/gen_ndr/xattr.h"
#include "libcli/security/security.h"


/* the list of currently registered ACL backends */
static struct pvfs_acl_backend {
	const struct pvfs_acl_ops *ops;
} *backends = NULL;
static int num_backends;

/*
  register a pvfs acl backend. 

  The 'name' can be later used by other backends to find the operations
  structure for this backend.  
*/
_PUBLIC_ NTSTATUS pvfs_acl_register(const struct pvfs_acl_ops *ops)
{
	struct pvfs_acl_ops *new_ops;

	if (pvfs_acl_backend_byname(ops->name) != NULL) {
		DEBUG(0,("pvfs acl backend '%s' already registered\n", ops->name));
		return NT_STATUS_OBJECT_NAME_COLLISION;
	}

	backends = talloc_realloc(talloc_autofree_context(), backends, struct pvfs_acl_backend, num_backends+1);
	NT_STATUS_HAVE_NO_MEMORY(backends);

	new_ops = talloc_memdup(backends, ops, sizeof(*ops));
	new_ops->name = talloc_strdup(new_ops, ops->name);

	backends[num_backends].ops = new_ops;

	num_backends++;

	DEBUG(3,("NTVFS backend '%s' registered\n", ops->name));

	return NT_STATUS_OK;
}


/*
  return the operations structure for a named backend
*/
_PUBLIC_ const struct pvfs_acl_ops *pvfs_acl_backend_byname(const char *name)
{
	int i;

	for (i=0;i<num_backends;i++) {
		if (strcmp(backends[i].ops->name, name) == 0) {
			return backends[i].ops;
		}
	}

	return NULL;
}


/*
  map a single access_mask from generic to specific bits for files/dirs
*/
static uint32_t pvfs_translate_mask(uint32_t access_mask)
{
	if (access_mask & SEC_MASK_GENERIC) {
		if (access_mask & SEC_GENERIC_READ)    access_mask |= SEC_RIGHTS_FILE_READ;
		if (access_mask & SEC_GENERIC_WRITE)   access_mask |= SEC_RIGHTS_FILE_WRITE;
		if (access_mask & SEC_GENERIC_EXECUTE) access_mask |= SEC_RIGHTS_FILE_EXECUTE;
		if (access_mask & SEC_GENERIC_ALL)     access_mask |= SEC_RIGHTS_FILE_ALL;
		access_mask &= ~SEC_MASK_GENERIC;
	}
	return access_mask;
}


/*
  map any generic access bits in the given acl
  this relies on the fact that the mappings for files and directories
  are the same
*/
static void pvfs_translate_generic_bits(struct security_acl *acl)
{
	unsigned i;

	if (!acl) return;

	for (i=0;i<acl->num_aces;i++) {
		struct security_ace *ace = &acl->aces[i];
		ace->access_mask = pvfs_translate_mask(ace->access_mask);
	}
}


/*
  setup a default ACL for a file
*/
static NTSTATUS pvfs_default_acl(struct pvfs_state *pvfs,
				 struct ntvfs_request *req,
				 struct pvfs_filename *name, int fd, 
				 struct security_descriptor **psd)
{
	struct security_descriptor *sd;
	NTSTATUS status;
	struct security_ace ace;
	mode_t mode;

	*psd = security_descriptor_initialise(req);
	if (*psd == NULL) {
		return NT_STATUS_NO_MEMORY;
	}
	sd = *psd;

	status = sidmap_uid_to_sid(pvfs->sidmap, sd, name->st.st_uid, &sd->owner_sid);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}
	status = sidmap_gid_to_sid(pvfs->sidmap, sd, name->st.st_gid, &sd->group_sid);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	sd->type |= SEC_DESC_DACL_PRESENT;

	mode = name->st.st_mode;

	/*
	  we provide up to 4 ACEs
	    - Owner
	    - Group
	    - Everyone
	    - Administrator
	 */


	/* setup owner ACE */
	ace.type = SEC_ACE_TYPE_ACCESS_ALLOWED;
	ace.flags = 0;
	ace.trustee = *sd->owner_sid;
	ace.access_mask = 0;

	if (mode & S_IRUSR) {
		if (mode & S_IWUSR) {
			ace.access_mask |= SEC_RIGHTS_FILE_ALL;
		} else {
			ace.access_mask |= SEC_RIGHTS_FILE_READ | SEC_FILE_EXECUTE;
		}
	}
	if (mode & S_IWUSR) {
		ace.access_mask |= SEC_RIGHTS_FILE_WRITE | SEC_STD_DELETE;
	}
	if (ace.access_mask) {
		security_descriptor_dacl_add(sd, &ace);
	}


	/* setup group ACE */
	ace.trustee = *sd->group_sid;
	ace.access_mask = 0;
	if (mode & S_IRGRP) {
		ace.access_mask |= SEC_RIGHTS_FILE_READ | SEC_FILE_EXECUTE;
	}
	if (mode & S_IWGRP) {
		/* note that delete is not granted - this matches posix behaviour */
		ace.access_mask |= SEC_RIGHTS_FILE_WRITE;
	}
	if (ace.access_mask) {
		security_descriptor_dacl_add(sd, &ace);
	}

	/* setup other ACE */
	ace.trustee = *dom_sid_parse_talloc(req, SID_WORLD);
	ace.access_mask = 0;
	if (mode & S_IROTH) {
		ace.access_mask |= SEC_RIGHTS_FILE_READ | SEC_FILE_EXECUTE;
	}
	if (mode & S_IWOTH) {
		ace.access_mask |= SEC_RIGHTS_FILE_WRITE;
	}
	if (ace.access_mask) {
		security_descriptor_dacl_add(sd, &ace);
	}

	/* setup system ACE */
	ace.trustee = *dom_sid_parse_talloc(req, SID_NT_SYSTEM);
	ace.access_mask = SEC_RIGHTS_FILE_ALL;
	security_descriptor_dacl_add(sd, &ace);
	
	return NT_STATUS_OK;
}
				 

/*
  omit any security_descriptor elements not specified in the given
  secinfo flags
*/
static void normalise_sd_flags(struct security_descriptor *sd, uint32_t secinfo_flags)
{
	if (!(secinfo_flags & SECINFO_OWNER)) {
		sd->owner_sid = NULL;
	}
	if (!(secinfo_flags & SECINFO_GROUP)) {
		sd->group_sid = NULL;
	}
	if (!(secinfo_flags & SECINFO_DACL)) {
		sd->dacl = NULL;
	}
	if (!(secinfo_flags & SECINFO_SACL)) {
		sd->sacl = NULL;
	}
}

/*
  answer a setfileinfo for an ACL
*/
NTSTATUS pvfs_acl_set(struct pvfs_state *pvfs, 
		      struct ntvfs_request *req,
		      struct pvfs_filename *name, int fd, 
		      uint32_t access_mask,
		      union smb_setfileinfo *info)
{
	uint32_t secinfo_flags = info->set_secdesc.in.secinfo_flags;
	struct security_descriptor *new_sd, *sd, orig_sd;
	NTSTATUS status = NT_STATUS_NOT_FOUND;
	uid_t old_uid = -1;
	gid_t old_gid = -1;
	uid_t new_uid = -1;
	gid_t new_gid = -1;

	if (pvfs->acl_ops != NULL) {
		status = pvfs->acl_ops->acl_load(pvfs, name, fd, req, &sd);
	}
	if (NT_STATUS_EQUAL(status, NT_STATUS_NOT_FOUND)) {
		status = pvfs_default_acl(pvfs, req, name, fd, &sd);
	}
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	new_sd = info->set_secdesc.in.sd;
	orig_sd = *sd;

	old_uid = name->st.st_uid;
	old_gid = name->st.st_gid;

	/* only set the elements that have been specified */
	if (secinfo_flags & SECINFO_OWNER) {
		if (!(access_mask & SEC_STD_WRITE_OWNER)) {
			return NT_STATUS_ACCESS_DENIED;
		}
		if (!dom_sid_equal(sd->owner_sid, new_sd->owner_sid)) {
			status = sidmap_sid_to_unixuid(pvfs->sidmap, new_sd->owner_sid, &new_uid);
			NT_STATUS_NOT_OK_RETURN(status);
		}
		sd->owner_sid = new_sd->owner_sid;
	}
	if (secinfo_flags & SECINFO_GROUP) {
		if (!(access_mask & SEC_STD_WRITE_OWNER)) {
			return NT_STATUS_ACCESS_DENIED;
		}
		if (!dom_sid_equal(sd->group_sid, new_sd->group_sid)) {
			status = sidmap_sid_to_unixgid(pvfs->sidmap, new_sd->group_sid, &new_gid);
			NT_STATUS_NOT_OK_RETURN(status);
		}
		sd->group_sid = new_sd->group_sid;
	}
	if (secinfo_flags & SECINFO_DACL) {
		if (!(access_mask & SEC_STD_WRITE_DAC)) {
			return NT_STATUS_ACCESS_DENIED;
		}
		sd->dacl = new_sd->dacl;
		pvfs_translate_generic_bits(sd->dacl);
	}
	if (secinfo_flags & SECINFO_SACL) {
		if (!(access_mask & SEC_FLAG_SYSTEM_SECURITY)) {
			return NT_STATUS_ACCESS_DENIED;
		}
		sd->sacl = new_sd->sacl;
		pvfs_translate_generic_bits(sd->sacl);
	}

	if (new_uid == old_uid) {
		new_uid = -1;
	}

	if (new_gid == old_gid) {
		new_gid = -1;
	}

	/* if there's something to change try it */
	if (new_uid != -1 || new_gid != -1) {
		int ret;
		if (fd == -1) {
			ret = chown(name->full_name, new_uid, new_gid);
		} else {
			ret = fchown(fd, new_uid, new_gid);
		}
		if (ret == -1) {
			return pvfs_map_errno(pvfs, errno);
		}
	}

	/* we avoid saving if the sd is the same. This means when clients
	   copy files and end up copying the default sd that we don't
	   needlessly use xattrs */
	if (!security_descriptor_equal(sd, &orig_sd) && pvfs->acl_ops) {
		status = pvfs->acl_ops->acl_save(pvfs, name, fd, sd);
	}

	return status;
}


/*
  answer a fileinfo query for the ACL
*/
NTSTATUS pvfs_acl_query(struct pvfs_state *pvfs, 
			struct ntvfs_request *req,
			struct pvfs_filename *name, int fd, 
			union smb_fileinfo *info)
{
	NTSTATUS status = NT_STATUS_NOT_FOUND;
	struct security_descriptor *sd;

	if (pvfs->acl_ops) {
		status = pvfs->acl_ops->acl_load(pvfs, name, fd, req, &sd);
	}
	if (NT_STATUS_EQUAL(status, NT_STATUS_NOT_FOUND)) {
		status = pvfs_default_acl(pvfs, req, name, fd, &sd);
	}
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	normalise_sd_flags(sd, info->query_secdesc.in.secinfo_flags);

	info->query_secdesc.out.sd = sd;

	return NT_STATUS_OK;
}


/*
  check the read only bit against any of the write access bits
*/
static BOOL pvfs_read_only(struct pvfs_state *pvfs, uint32_t access_mask)
{
	if ((pvfs->flags & PVFS_FLAG_READONLY) &&
	    (access_mask & (SEC_FILE_WRITE_DATA |
			    SEC_FILE_APPEND_DATA | 
			    SEC_FILE_WRITE_EA | 
			    SEC_FILE_WRITE_ATTRIBUTE | 
			    SEC_STD_DELETE | 
			    SEC_STD_WRITE_DAC | 
			    SEC_STD_WRITE_OWNER | 
			    SEC_DIR_DELETE_CHILD))) {
		return True;
	}
	return False;
}

/*
  default access check function based on unix permissions
  doing this saves on building a full security descriptor
  for the common case of access check on files with no 
  specific NT ACL
*/
NTSTATUS pvfs_access_check_unix(struct pvfs_state *pvfs, 
				struct ntvfs_request *req,
				struct pvfs_filename *name,
				uint32_t *access_mask)
{
	uid_t uid = geteuid();
	uint32_t max_bits = SEC_RIGHTS_FILE_READ | SEC_FILE_ALL;

	if (pvfs_read_only(pvfs, *access_mask)) {
		return NT_STATUS_ACCESS_DENIED;
	}

	/* owner and root get extra permissions */
	if (uid == 0) {
		max_bits |= SEC_STD_ALL | SEC_FLAG_SYSTEM_SECURITY;
	} else if (uid == name->st.st_uid) {
		max_bits |= SEC_STD_ALL;
	}

	if (*access_mask == SEC_FLAG_MAXIMUM_ALLOWED) {
		*access_mask = max_bits;
		return NT_STATUS_OK;
	}

	if (uid != 0 && (*access_mask & SEC_FLAG_SYSTEM_SECURITY)) {
		return NT_STATUS_PRIVILEGE_NOT_HELD;
	}

	if (*access_mask & ~max_bits) {
		return NT_STATUS_ACCESS_DENIED;
	}

	*access_mask |= SEC_FILE_READ_ATTRIBUTE;

	return NT_STATUS_OK;
}


/*
  check the security descriptor on a file, if any
  
  *access_mask is modified with the access actually granted
*/
NTSTATUS pvfs_access_check(struct pvfs_state *pvfs, 
			   struct ntvfs_request *req,
			   struct pvfs_filename *name,
			   uint32_t *access_mask)
{
	struct security_token *token = req->session_info->security_token;
	struct xattr_NTACL *acl;
	NTSTATUS status;
	struct security_descriptor *sd;

	if (pvfs_read_only(pvfs, *access_mask)) {
		return NT_STATUS_ACCESS_DENIED;
	}

	acl = talloc(req, struct xattr_NTACL);
	if (acl == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	/* expand the generic access bits to file specific bits */
	*access_mask = pvfs_translate_mask(*access_mask);
	*access_mask &= ~SEC_FILE_READ_ATTRIBUTE;

	status = pvfs_acl_load(pvfs, name, -1, acl);
	if (NT_STATUS_EQUAL(status, NT_STATUS_NOT_FOUND)) {
		talloc_free(acl);
		return pvfs_access_check_unix(pvfs, req, name, access_mask);
	}
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	switch (acl->version) {
	case 1:
		sd = acl->info.sd;
		break;
	default:
		return NT_STATUS_INVALID_ACL;
	}

	/* check the acl against the required access mask */
	status = sec_access_check(sd, token, *access_mask, access_mask);

	/* this bit is always granted, even if not asked for */
	*access_mask |= SEC_FILE_READ_ATTRIBUTE;

	talloc_free(acl);
	
	return status;
}


/*
  a simplified interface to access check, designed for calls that
  do not take or return an access check mask
*/
NTSTATUS pvfs_access_check_simple(struct pvfs_state *pvfs, 
				  struct ntvfs_request *req,
				  struct pvfs_filename *name,
				  uint32_t access_needed)
{
	if (access_needed == 0) {
		return NT_STATUS_OK;
	}
	return pvfs_access_check(pvfs, req, name, &access_needed);
}

/*
  access check for creating a new file/directory
*/
NTSTATUS pvfs_access_check_create(struct pvfs_state *pvfs, 
				  struct ntvfs_request *req,
				  struct pvfs_filename *name,
				  uint32_t *access_mask)
{
	struct pvfs_filename *parent;
	NTSTATUS status;

	status = pvfs_resolve_parent(pvfs, req, name, &parent);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	status = pvfs_access_check(pvfs, req, parent, access_mask);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (! ((*access_mask) & SEC_DIR_ADD_FILE)) {
		return pvfs_access_check_simple(pvfs, req, parent, SEC_DIR_ADD_FILE);
	}

	return status;
}

/*
  access check for creating a new file/directory - no access mask supplied
*/
NTSTATUS pvfs_access_check_parent(struct pvfs_state *pvfs, 
				  struct ntvfs_request *req,
				  struct pvfs_filename *name,
				  uint32_t access_mask)
{
	struct pvfs_filename *parent;
	NTSTATUS status;

	status = pvfs_resolve_parent(pvfs, req, name, &parent);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	return pvfs_access_check_simple(pvfs, req, parent, access_mask);
}


/*
  determine if an ACE is inheritable
*/
static BOOL pvfs_inheritable_ace(struct pvfs_state *pvfs,
				 const struct security_ace *ace,
				 BOOL container)
{
	if (!container) {
		return (ace->flags & SEC_ACE_FLAG_OBJECT_INHERIT) != 0;
	}

	if (ace->flags & SEC_ACE_FLAG_CONTAINER_INHERIT) {
		return True;
	}

	if ((ace->flags & SEC_ACE_FLAG_OBJECT_INHERIT) &&
	    !(ace->flags & SEC_ACE_FLAG_NO_PROPAGATE_INHERIT)) {
		return True;
	}

	return False;
}

/*
  this is the core of ACL inheritance. It copies any inheritable
  aces from the parent SD to the child SD. Note that the algorithm 
  depends on whether the child is a container or not
*/
static NTSTATUS pvfs_acl_inherit_aces(struct pvfs_state *pvfs, 
				      struct security_descriptor *parent_sd,
				      struct security_descriptor *sd,
				      BOOL container)
{
	int i;
	
	for (i=0;i<parent_sd->dacl->num_aces;i++) {
		struct security_ace ace = parent_sd->dacl->aces[i];
		NTSTATUS status;
		const struct dom_sid *creator = NULL, *new_id = NULL;
		uint32_t orig_flags;

		if (!pvfs_inheritable_ace(pvfs, &ace, container)) {
			continue;
		}

		orig_flags = ace.flags;

		/* see the RAW-ACLS inheritance test for details on these rules */
		if (!container) {
			ace.flags = 0;
		} else {
			ace.flags &= ~SEC_ACE_FLAG_INHERIT_ONLY;

			if (!(ace.flags & SEC_ACE_FLAG_CONTAINER_INHERIT)) {
				ace.flags |= SEC_ACE_FLAG_INHERIT_ONLY;
			}
			if (ace.flags & SEC_ACE_FLAG_NO_PROPAGATE_INHERIT) {
				ace.flags = 0;
			}
		}

		/* the CREATOR sids are special when inherited */
		if (dom_sid_equal(&ace.trustee, pvfs->sid_cache.creator_owner)) {
			creator = pvfs->sid_cache.creator_owner;
			new_id = sd->owner_sid;
		} else if (dom_sid_equal(&ace.trustee, pvfs->sid_cache.creator_group)) {
			creator = pvfs->sid_cache.creator_group;
			new_id = sd->group_sid;
		} else {
			new_id = &ace.trustee;
		}

		if (creator && container && 
		    (ace.flags & SEC_ACE_FLAG_CONTAINER_INHERIT)) {
			uint32_t flags = ace.flags;

			ace.trustee = *new_id;
			ace.flags = 0;
			status = security_descriptor_dacl_add(sd, &ace);
			if (!NT_STATUS_IS_OK(status)) {
				return status;
			}

			ace.trustee = *creator;
			ace.flags = flags | SEC_ACE_FLAG_INHERIT_ONLY;
			status = security_descriptor_dacl_add(sd, &ace);
		} else if (container && 
			   !(orig_flags & SEC_ACE_FLAG_NO_PROPAGATE_INHERIT)) {
			status = security_descriptor_dacl_add(sd, &ace);
		} else {
			ace.trustee = *new_id;
			status = security_descriptor_dacl_add(sd, &ace);
		}

		if (!NT_STATUS_IS_OK(status)) {
			return status;
		}
	}

	return NT_STATUS_OK;
}



/*
  setup an ACL on a new file/directory based on the inherited ACL from
  the parent. If there is no inherited ACL then we don't set anything,
  as the default ACL applies anyway
*/
NTSTATUS pvfs_acl_inherit(struct pvfs_state *pvfs, 
			  struct ntvfs_request *req,
			  struct pvfs_filename *name,
			  int fd)
{
	struct xattr_NTACL *acl;
	NTSTATUS status;
	struct pvfs_filename *parent;
	struct security_descriptor *parent_sd, *sd;
	BOOL container;

	/* form the parents path */
	status = pvfs_resolve_parent(pvfs, req, name, &parent);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	acl = talloc(req, struct xattr_NTACL);
	if (acl == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	status = pvfs_acl_load(pvfs, parent, -1, acl);
	if (NT_STATUS_EQUAL(status, NT_STATUS_NOT_FOUND)) {
		return NT_STATUS_OK;
	}
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	switch (acl->version) {
	case 1:
		parent_sd = acl->info.sd;
		break;
	default:
		return NT_STATUS_INVALID_ACL;
	}

	if (parent_sd == NULL ||
	    parent_sd->dacl == NULL ||
	    parent_sd->dacl->num_aces == 0) {
		/* go with the default ACL */
		return NT_STATUS_OK;
	}

	/* create the new sd */
	sd = security_descriptor_initialise(req);
	if (sd == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	status = sidmap_uid_to_sid(pvfs->sidmap, sd, name->st.st_uid, &sd->owner_sid);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}
	status = sidmap_gid_to_sid(pvfs->sidmap, sd, name->st.st_gid, &sd->group_sid);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	sd->type |= SEC_DESC_DACL_PRESENT;

	container = (name->dos.attrib & FILE_ATTRIBUTE_DIRECTORY) ? True:False;

	/* fill in the aces from the parent */
	status = pvfs_acl_inherit_aces(pvfs, parent_sd, sd, container);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	/* if there is nothing to inherit then we fallback to the
	   default acl */
	if (sd->dacl == NULL || sd->dacl->num_aces == 0) {
		return NT_STATUS_OK;
	}

	acl->info.sd = sd;

	status = pvfs_acl_save(pvfs, name, fd, acl);
	
	return status;
}
