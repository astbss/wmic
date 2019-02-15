/*
 *	@file 	mprUnix.h
 *	@brief 	Make windows a bit more unix like
 *	@copy	default
 *	
 *	Copyright (c) Mbedthis Software LLC, 2003-2006. All Rights Reserved.
 *	
 *	This software is distributed under commercial and open source licenses.
 *	You may use the GPL open source license described below or you may acquire 
 *	a commercial license from Mbedthis Software. You agree to be fully bound 
 *	by the terms of either license. Consult the LICENSE.TXT distributed with 
 *	this software for full details.
 *	
 *	This software is open source; you can redistribute it and/or modify it 
 *	under the terms of the GNU General Public License as published by the 
 *	Free Software Foundation; either version 2 of the License, or (at your 
 *	option) any later version. See the GNU General Public License for more 
 *	details at: http://www.mbedthis.com/downloads/gplLicense.html
 *	
 *	This program is distributed WITHOUT ANY WARRANTY; without even the 
 *	implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 
 *	
 *	This GPL license does NOT permit incorporating this software into 
 *	proprietary programs. If you are unable to comply with the GPL, you must
 *	acquire a commercial license to use this software. Commercial licenses 
 *	for this software and support services are available from Mbedthis 
 *	Software at http://www.mbedthis.com 
 *	
 *	@end
 */

/******************************* Documentation ********************************/

/*
 *	This header is part of the Mbedthis Portable Runtime and aims to include
 *	all necessary O/S headers and to unify the constants and declarations 
 *	required by Mbedthis products. It can be included by C or C++ programs.
 */

/******************************************************************************/

#ifndef _h_MPR_UNIX
#define _h_MPR_UNIX 1

/******************************************************************************/
#ifdef __cplusplus
extern "C" {
#endif

/*
 *	Define BLD_NO_POSIX_REMAP if these defines mess with your app
 */
#if WIN && !BLD_NO_POSIX_REMAP
/*
 *	MOB -- clashes with ATL
 */
#define access 	_access
#define close 	_close
#define fileno 	_fileno
#define fstat 	_fstat
#define getpid 	_getpid
#define open 	_open
#define putenv 	_putenv
#define read 	_read
#define stat 	_stat
#define umask 	_umask
#define unlink 	_unlink
#define write 	_write
#define strdup 	_strdup
#define lseek 	_lseek
#define getcwd 	_getcwd
#define chdir 	_chdir

#define mkdir(a,b) 	_mkdir(a)
#define rmdir(a) 	_rmdir(a)

#define 	R_OK		4
#define 	W_OK		2
#define		MPR_TEXT	"t"

extern void		srand48(long);
extern long		lrand48(void);
extern long 	ulimit(int, ...);
extern long 	nap(long);
extern int	 	getuid(void);
extern int	 	geteuid(void);
#endif


/******************************************************************************/
#ifdef __cplusplus
}
#endif

#endif /* _h_MPR_UNIX */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim:tw=78
 * vim600: sw=4 ts=4 fdm=marker
 * vim<600: sw=4 ts=4
 */
