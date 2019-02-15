/* 
   Unix SMB/CIFS implementation.

   POSIX NTVFS backend - 

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
#include "vfs_posix.h"

/****************************************************************************
 Change a unix mode to a dos mode.
****************************************************************************/
static uint32_t dos_mode_from_stat(struct pvfs_state *pvfs, struct stat *st)
{
	int result = 0;

	if ((st->st_mode & S_IWUSR) == 0)
		result |= FILE_ATTRIBUTE_READONLY;
	
	if ((pvfs->flags & PVFS_FLAG_MAP_ARCHIVE) && ((st->st_mode & S_IXUSR) != 0))
		result |= FILE_ATTRIBUTE_ARCHIVE;
	
	if ((pvfs->flags & PVFS_FLAG_MAP_SYSTEM) && ((st->st_mode & S_IXGRP) != 0))
		result |= FILE_ATTRIBUTE_SYSTEM;
	
	if ((pvfs->flags & PVFS_FLAG_MAP_HIDDEN) && ((st->st_mode & S_IXOTH) != 0))
		result |= FILE_ATTRIBUTE_HIDDEN;
  
	if (S_ISDIR(st->st_mode))
		result = FILE_ATTRIBUTE_DIRECTORY | (result & FILE_ATTRIBUTE_READONLY);

	return result;
}



/*
  fill in the dos file attributes for a file
*/
NTSTATUS pvfs_fill_dos_info(struct pvfs_state *pvfs, struct pvfs_filename *name, int fd)
{
	/* make directories appear as size 0 with 1 link */
	if (S_ISDIR(name->st.st_mode)) {
		name->st.st_size = 0;
		name->st.st_nlink = 1;
	}

	/* for now just use the simple samba mapping */
	unix_to_nt_time(&name->dos.create_time, name->st.st_ctime);
	unix_to_nt_time(&name->dos.access_time, name->st.st_atime);
	unix_to_nt_time(&name->dos.write_time,  name->st.st_mtime);
	unix_to_nt_time(&name->dos.change_time, name->st.st_ctime);
#ifdef HAVE_STAT_TV_NSEC
	name->dos.create_time += name->st.st_ctim.tv_nsec / 100;
	name->dos.access_time += name->st.st_atim.tv_nsec / 100;
	name->dos.write_time  += name->st.st_mtim.tv_nsec / 100;
	name->dos.change_time += name->st.st_ctim.tv_nsec / 100;
#endif
	name->dos.attrib = dos_mode_from_stat(pvfs, &name->st);
	name->dos.alloc_size = pvfs_round_alloc_size(pvfs, name->st.st_size);
	name->dos.nlink = name->st.st_nlink;
	name->dos.ea_size = 4;
	name->dos.file_id = (((uint64_t)name->st.st_dev)<<32) | name->st.st_ino;
	name->dos.flags = 0;

	return pvfs_dosattrib_load(pvfs, name, fd);
}


/*
  return a set of unix file permissions for a new file or directory
*/
mode_t pvfs_fileperms(struct pvfs_state *pvfs, uint32_t attrib)
{
	mode_t mode = S_IRUSR;

	if (attrib & FILE_ATTRIBUTE_DIRECTORY) {
		mode |= S_IXUSR;
	}

	if (!(attrib & FILE_ATTRIBUTE_READONLY) ||
	    (pvfs->flags & PVFS_FLAG_XATTR_ENABLE)) {
		mode |= S_IWUSR;
	}

	if (!(pvfs->flags & PVFS_FLAG_XATTR_ENABLE)) {
		if ((attrib & FILE_ATTRIBUTE_ARCHIVE) &&
		    (pvfs->flags & PVFS_FLAG_MAP_ARCHIVE)) {
			mode |= S_IXUSR;
		}
		
		if ((attrib & FILE_ATTRIBUTE_SYSTEM) &&
		    (pvfs->flags & PVFS_FLAG_MAP_SYSTEM)) {
			mode |= S_IXGRP;
		}
		
		if ((attrib & FILE_ATTRIBUTE_HIDDEN) &&
		    (pvfs->flags & PVFS_FLAG_MAP_HIDDEN)) {
			mode |= S_IXOTH;
		}
	}

	return mode;
}

