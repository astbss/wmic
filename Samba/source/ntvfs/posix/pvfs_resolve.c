/* 
   Unix SMB/CIFS implementation.

   POSIX NTVFS backend - filename resolution

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

/*
  this is the core code for converting a filename from the format as
  given by a client to a posix filename, including any case-matching
  required, and checks for legal characters
*/


#include "includes.h"
#include "vfs_posix.h"
#include "system/dir.h"

/*
  compare two filename components. This is where the name mangling hook will go
*/
static int component_compare(struct pvfs_state *pvfs, const char *comp, const char *name)
{
	int ret;

	ret = strcasecmp_m(comp, name);

	if (ret != 0) {
		char *shortname = pvfs_short_name_component(pvfs, name);
		if (shortname) {
			ret = strcasecmp_m(comp, shortname);
			talloc_free(shortname);
		}
	}

	return ret;
}

/*
  search for a filename in a case insensitive fashion

  TODO: add a cache for previously resolved case-insensitive names
  TODO: add mangled name support
*/
static NTSTATUS pvfs_case_search(struct pvfs_state *pvfs, struct pvfs_filename *name)
{
	/* break into a series of components */
	int num_components;
	char **components;
	char *p, *partial_name;
	int i;

	/* break up the full name info pathname components */
	num_components=2;
	p = name->full_name + strlen(pvfs->base_directory) + 1;

	for (;*p;p++) {
		if (*p == '/') {
			num_components++;
		}
	}

	components = talloc_array(name, char *, num_components);
	p = name->full_name + strlen(pvfs->base_directory);
	*p++ = 0;

	components[0] = name->full_name;

	for (i=1;i<num_components;i++) {
		components[i] = p;
		p = strchr(p, '/');
		if (p) *p++ = 0;
		if (pvfs_is_reserved_name(pvfs, components[i])) {
			return NT_STATUS_ACCESS_DENIED;
		}
	}

	partial_name = talloc_strdup(name, components[0]);
	if (!partial_name) {
		return NT_STATUS_NO_MEMORY;
	}

	/* for each component, check if it exists as-is, and if not then
	   do a directory scan */
	for (i=1;i<num_components;i++) {
		char *test_name;
		DIR *dir;
		struct dirent *de;
		char *long_component;

		/* possibly remap from the short name cache */
		long_component = pvfs_mangled_lookup(pvfs, name, components[i]);
		if (long_component) {
			components[i] = long_component;
		}

		test_name = talloc_asprintf(name, "%s/%s", partial_name, components[i]);
		if (!test_name) {
			return NT_STATUS_NO_MEMORY;
		}

		/* check if this component exists as-is */
		if (stat(test_name, &name->st) == 0) {
			if (i<num_components-1 && !S_ISDIR(name->st.st_mode)) {
				return NT_STATUS_OBJECT_PATH_NOT_FOUND;
			}
			talloc_free(partial_name);
			partial_name = test_name;
			if (i == num_components - 1) {
				name->exists = True;
			}
			continue;
		}

		/* the filesystem might be case insensitive, in which
		   case a search is pointless unless the name is
		   mangled */
		if ((pvfs->flags & PVFS_FLAG_CI_FILESYSTEM) &&
		    !pvfs_is_mangled_component(pvfs, components[i])) {
			if (i < num_components-1) {
				return NT_STATUS_OBJECT_PATH_NOT_FOUND;
			}
			partial_name = test_name;
			continue;
		}
		
		dir = opendir(partial_name);
		if (!dir) {
			return pvfs_map_errno(pvfs, errno);
		}

		while ((de = readdir(dir))) {
			if (component_compare(pvfs, components[i], de->d_name) == 0) {
				break;
			}
		}

		if (!de) {
			if (i < num_components-1) {
				closedir(dir);
				return NT_STATUS_OBJECT_PATH_NOT_FOUND;
			}
		} else {
			components[i] = talloc_strdup(name, de->d_name);
		}
		test_name = talloc_asprintf(name, "%s/%s", partial_name, components[i]);
		talloc_free(partial_name);
		partial_name = test_name;

		closedir(dir);
	}

	if (!name->exists) {
		if (stat(partial_name, &name->st) == 0) {
			name->exists = True;
		}
	}

	talloc_free(name->full_name);
	name->full_name = partial_name;

	if (name->exists) {
		return pvfs_fill_dos_info(pvfs, name, -1);
	}

	return NT_STATUS_OK;
}

/*
  parse a alternate data stream name
*/
static NTSTATUS parse_stream_name(struct pvfs_filename *name, const char *s)
{
	char *p;
	name->stream_name = talloc_strdup(name, s+1);
	if (name->stream_name == NULL) {
		return NT_STATUS_NO_MEMORY;
	}
	p = strchr_m(name->stream_name, ':');
	if (p == NULL) {
		name->stream_id = pvfs_name_hash(name->stream_name, 
						 strlen(name->stream_name));
		return NT_STATUS_OK;
	}
	if (strcasecmp_m(p, ":$DATA") != 0) {
		return NT_STATUS_OBJECT_NAME_INVALID;
	}
	*p = 0;
	if (strcmp(name->stream_name, "") == 0) {
		name->stream_name = NULL;
		name->stream_id = 0;
	} else {
		name->stream_id = pvfs_name_hash(name->stream_name, 
						 strlen(name->stream_name));
	}
						 
	return NT_STATUS_OK;	
}


/*
  convert a CIFS pathname to a unix pathname. Note that this does NOT
  take into account case insensitivity, and in fact does not access
  the filesystem at all. It is merely a reformatting and charset
  checking routine.

  errors are returned if the filename is illegal given the flags
*/
static NTSTATUS pvfs_unix_path(struct pvfs_state *pvfs, const char *cifs_name,
			       uint_t flags, struct pvfs_filename *name)
{
	char *ret, *p, *p_start;
	NTSTATUS status;

	name->original_name = talloc_strdup(name, cifs_name);
	name->stream_name = NULL;
	name->stream_id = 0;
	name->has_wildcard = False;

	while (*cifs_name == '\\') {
		cifs_name++;
	}

	if (*cifs_name == 0) {
		name->full_name = talloc_asprintf(name, "%s/.", pvfs->base_directory);
		if (name->full_name == NULL) {
			return NT_STATUS_NO_MEMORY;
		}
		return NT_STATUS_OK;
	}

	ret = talloc_asprintf(name, "%s/%s", pvfs->base_directory, cifs_name);
	if (ret == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	p = ret + strlen(pvfs->base_directory) + 1;

	/* now do an in-place conversion of '\' to '/', checking
	   for legal characters */
	p_start = p;

	while (*p) {
		size_t c_size;
		codepoint_t c = next_codepoint(p, &c_size);
		switch (c) {
		case '\\':
			if (name->has_wildcard) {
				/* wildcards are only allowed in the last part
				   of a name */
				return NT_STATUS_ILLEGAL_CHARACTER;
			}
			if (p > p_start && p[1] == 0) {
				*p = 0;
			} else {
				*p = '/';
			}
			break;
		case ':':
			if (!(flags & PVFS_RESOLVE_STREAMS)) {
				return NT_STATUS_ILLEGAL_CHARACTER;
			}
			if (name->has_wildcard) {
				return NT_STATUS_ILLEGAL_CHARACTER;
			}
			status = parse_stream_name(name, p);
			if (!NT_STATUS_IS_OK(status)) {
				return status;
			}
			*p-- = 0;
			break;
		case '*':
		case '>':
		case '<':
		case '?':
		case '"':
			if (!(flags & PVFS_RESOLVE_WILDCARD)) {
				return NT_STATUS_OBJECT_NAME_INVALID;
			}
			name->has_wildcard = True;
			break;
		case '/':
		case '|':
			return NT_STATUS_ILLEGAL_CHARACTER;
		case '.':
			/* see if it is definately a .. or
			   . component. If it is then fail here, and
			   let the next layer up try again after
			   pvfs_reduce_name() if it wants to. This is
			   much more efficient on average than always
			   scanning for these separately */
			if (p[1] == '.' && 
			    (p[2] == 0 || p[2] == '\\') &&
			    (p == p_start || p[-1] == '/')) {
				return NT_STATUS_OBJECT_PATH_SYNTAX_BAD;
			}
			if ((p[1] == 0 || p[1] == '\\') &&
			    (p == p_start || p[-1] == '/')) {
				return NT_STATUS_OBJECT_PATH_SYNTAX_BAD;
			}
			break;
		}

		p += c_size;
	}

	name->full_name = ret;

	return NT_STATUS_OK;
}


/*
  reduce a name that contains .. components or repeated \ separators
  return NULL if it can't be reduced
*/
static NTSTATUS pvfs_reduce_name(TALLOC_CTX *mem_ctx, const char **fname, uint_t flags)
{
	codepoint_t c;
	size_t c_size, len;
	int i, num_components, err_count;
	char **components;
	char *p, *s, *ret;

	s = talloc_strdup(mem_ctx, *fname);
	if (s == NULL) return NT_STATUS_NO_MEMORY;

	for (num_components=1, p=s; *p; p += c_size) {
		c = next_codepoint(p, &c_size);
		if (c == '\\') num_components++;
	}

	components = talloc_array(s, char *, num_components+1);
	if (components == NULL) {
		talloc_free(s);
		return NT_STATUS_NO_MEMORY;
	}

	components[0] = s;
	for (i=0, p=s; *p; p += c_size) {
		c = next_codepoint(p, &c_size);
		if (c == '\\') {
			*p = 0;
			components[++i] = p+1;
		}
	}
	components[i+1] = NULL;

	/*
	  rather bizarre!

	  '.' components are not allowed, but the rules for what error
	  code to give don't seem to make sense. This is a close
	  approximation.
	*/
	for (err_count=i=0;components[i];i++) {
		if (strcmp(components[i], "") == 0) {
			continue;
		}
		if (ISDOT(components[i]) || err_count) {
			err_count++;
		}
	}
	if (err_count) {
		if (flags & PVFS_RESOLVE_WILDCARD) err_count--;

		if (err_count==1) {
			return NT_STATUS_OBJECT_NAME_INVALID;
		} else {
			return NT_STATUS_OBJECT_PATH_NOT_FOUND;
		}
	}

	/* remove any null components */
	for (i=0;components[i];i++) {
		if (strcmp(components[i], "") == 0) {
			memmove(&components[i], &components[i+1], 
				sizeof(char *)*(num_components-i));
			i--;
			continue;
		}
		if (ISDOTDOT(components[i])) {
			if (i < 1) return NT_STATUS_OBJECT_PATH_SYNTAX_BAD;
			memmove(&components[i-1], &components[i+1], 
				sizeof(char *)*(num_components-(i+1)));
			i -= 2;
			continue;
		}
	}

	if (components[0] == NULL) {
		talloc_free(s);
		*fname = talloc_strdup(mem_ctx, "\\");
		return NT_STATUS_OK;
	}

	for (len=i=0;components[i];i++) {
		len += strlen(components[i]) + 1;
	}

	/* rebuild the name */
	ret = talloc_size(mem_ctx, len+1);
	if (ret == NULL) {
		talloc_free(s);
		return NT_STATUS_NO_MEMORY;
	}

	for (len=0,i=0;components[i];i++) {
		size_t len1 = strlen(components[i]);
		ret[len] = '\\';
		memcpy(ret+len+1, components[i], len1);
		len += len1 + 1;
	}	
	ret[len] = 0;

	talloc_free(s);

	*fname = ret;
	
	return NT_STATUS_OK;
}


/*
  resolve a name from relative client format to a struct pvfs_filename
  the memory for the filename is made as a talloc child of 'name'

  flags include:
     PVFS_RESOLVE_NO_WILDCARD = wildcards are considered illegal characters
     PVFS_RESOLVE_STREAMS     = stream names are allowed

     TODO: ../ collapsing, and outside share checking
*/
NTSTATUS pvfs_resolve_name(struct pvfs_state *pvfs, TALLOC_CTX *mem_ctx,
			   const char *cifs_name,
			   uint_t flags, struct pvfs_filename **name)
{
	NTSTATUS status;

	*name = talloc(mem_ctx, struct pvfs_filename);
	if (*name == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	(*name)->exists = False;
	(*name)->stream_exists = False;

	if (!(pvfs->fs_attribs & FS_ATTR_NAMED_STREAMS)) {
		flags &= ~PVFS_RESOLVE_STREAMS;
	}

	/* do the basic conversion to a unix formatted path,
	   also checking for allowable characters */
	status = pvfs_unix_path(pvfs, cifs_name, flags, *name);

	if (NT_STATUS_EQUAL(status, NT_STATUS_OBJECT_PATH_SYNTAX_BAD)) {
		/* it might contain .. components which need to be reduced */
		status = pvfs_reduce_name(*name, &cifs_name, flags);
		if (!NT_STATUS_IS_OK(status)) {
			return status;
		}
		status = pvfs_unix_path(pvfs, cifs_name, flags, *name);
	}

	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	/* if it has a wildcard then no point doing a stat() */
	if ((*name)->has_wildcard) {
		return NT_STATUS_OK;
	}

	/* if we can stat() the full name now then we are done */
	if (stat((*name)->full_name, &(*name)->st) == 0) {
		(*name)->exists = True;
		return pvfs_fill_dos_info(pvfs, *name, -1);
	}

	/* search for a matching filename */
	status = pvfs_case_search(pvfs, *name);

	return status;
}


/*
  do a partial resolve, returning a pvfs_filename structure given a
  base path and a relative component. It is an error if the file does
  not exist. No case-insensitive matching is done.

  this is used in places like directory searching where we need a pvfs_filename
  to pass to a function, but already know the unix base directory and component
*/
NTSTATUS pvfs_resolve_partial(struct pvfs_state *pvfs, TALLOC_CTX *mem_ctx,
			      const char *unix_dir, const char *fname,
			      struct pvfs_filename **name)
{
	NTSTATUS status;

	*name = talloc(mem_ctx, struct pvfs_filename);
	if (*name == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	(*name)->full_name = talloc_asprintf(*name, "%s/%s", unix_dir, fname);
	if ((*name)->full_name == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	if (stat((*name)->full_name, &(*name)->st) == -1) {
		return NT_STATUS_OBJECT_NAME_NOT_FOUND;
	}

	(*name)->exists = True;
	(*name)->stream_exists = True;
	(*name)->has_wildcard = False;
	(*name)->original_name = talloc_strdup(*name, fname);
	(*name)->stream_name = NULL;
	(*name)->stream_id = 0;

	status = pvfs_fill_dos_info(pvfs, *name, -1);

	return status;
}


/*
  fill in the pvfs_filename info for an open file, given the current
  info for a (possibly) non-open file. This is used by places that need
  to update the pvfs_filename stat information, and by pvfs_open()
*/
NTSTATUS pvfs_resolve_name_fd(struct pvfs_state *pvfs, int fd,
			      struct pvfs_filename *name)
{
	dev_t device = (dev_t)0;
	ino_t inode = 0;

	if (name->exists) {
		device = name->st.st_dev;
		inode = name->st.st_ino;
	}

	if (fd == -1) {
		if (stat(name->full_name, &name->st) == -1) {
			return NT_STATUS_INVALID_HANDLE;
		}
	} else {
		if (fstat(fd, &name->st) == -1) {
			return NT_STATUS_INVALID_HANDLE;
		}
	}

	if (name->exists &&
	    (device != name->st.st_dev || inode != name->st.st_ino)) {
		/* the file we are looking at has changed! this could
		 be someone trying to exploit a race
		 condition. Certainly we don't want to continue
		 operating on this file */
		DEBUG(0,("pvfs: WARNING: file '%s' changed during resolve - failing\n",
			 name->full_name));
		return NT_STATUS_UNEXPECTED_IO_ERROR;
	}

	name->exists = True;
	
	return pvfs_fill_dos_info(pvfs, name, fd);
}


/*
  resolve the parent of a given name
*/
NTSTATUS pvfs_resolve_parent(struct pvfs_state *pvfs, TALLOC_CTX *mem_ctx,
			     const struct pvfs_filename *child,
			     struct pvfs_filename **name)
{
	NTSTATUS status;
	char *p;

	*name = talloc(mem_ctx, struct pvfs_filename);
	if (*name == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	(*name)->full_name = talloc_strdup(*name, child->full_name);
	if ((*name)->full_name == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	p = strrchr_m((*name)->full_name, '/');
	if (p == NULL) {
		return NT_STATUS_OBJECT_PATH_SYNTAX_BAD;
	}

	/* this handles the root directory */
	if (p == (*name)->full_name) {
		p[1] = 0;
	} else {
		p[0] = 0;
	}

	if (stat((*name)->full_name, &(*name)->st) == -1) {
		return NT_STATUS_OBJECT_NAME_NOT_FOUND;
	}

	(*name)->exists = True;
	(*name)->stream_exists = True;
	(*name)->has_wildcard = False;
	/* we can't get the correct 'original_name', but for the purposes
	   of this call this is close enough */
	(*name)->original_name = talloc_reference(*name, child->original_name);
	(*name)->stream_name = NULL;
	(*name)->stream_id = 0;

	status = pvfs_fill_dos_info(pvfs, *name, -1);

	return status;
}
