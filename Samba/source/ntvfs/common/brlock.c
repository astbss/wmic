/* 
   Unix SMB/CIFS implementation.

   generic byte range locking code

   Copyright (C) Andrew Tridgell 1992-2004
   Copyright (C) Jeremy Allison 1992-2000
   
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

/* This module implements a tdb based byte range locking service,
   replacing the fcntl() based byte range locking previously
   used. This allows us to provide the same semantics as NT */

#include "includes.h"
#include "system/filesys.h"
#include "lib/tdb/include/tdb.h"
#include "messaging/messaging.h"
#include "db_wrap.h"
#include "lib/messaging/irpc.h"
#include "libcli/libcli.h"

/*
  in this module a "DATA_BLOB *file_key" is a blob that uniquely identifies
  a file. For a local posix filesystem this will usually be a combination
  of the device and inode numbers of the file, but it can be anything 
  that uniquely idetifies a file for locking purposes, as long
  as it is applied consistently.
*/

struct brl_context;
/*
  the lock context contains the elements that define whether one
  lock is the same as another lock
*/
struct lock_context {
	uint32_t server;
	uint16_t smbpid;
	struct brl_context *ctx;
};

/* The data in brlock records is an unsorted linear array of these
   records.  It is unnecessary to store the count as tdb provides the
   size of the record */
struct lock_struct {
	struct lock_context context;
	struct ntvfs_handle *ntvfs;
	uint64_t start;
	uint64_t size;
	enum brl_type lock_type;
	void *notify_ptr;
};

/* this struct is attached to on oprn file handle */
struct brl_handle {
	DATA_BLOB key;
	struct ntvfs_handle *ntvfs;
	struct lock_struct last_lock;
};

/* this struct is typicaly attached to tcon */
struct brl_context {
	struct tdb_wrap *w;
	uint32_t server;
	struct messaging_context *messaging_ctx;
};

/*
  Open up the brlock.tdb database. Close it down using
  talloc_free(). We need the messaging_ctx to allow for
  pending lock notifications.
*/
struct brl_context *brl_init(TALLOC_CTX *mem_ctx, uint32_t server, 
			     struct messaging_context *messaging_ctx)
{
	char *path;
	struct brl_context *brl;

	brl = talloc(mem_ctx, struct brl_context);
	if (brl == NULL) {
		return NULL;
	}

	path = smbd_tmp_path(brl, "brlock.tdb");
	brl->w = tdb_wrap_open(brl, path, 0,
			       TDB_DEFAULT, O_RDWR|O_CREAT, 0600);
	talloc_free(path);
	if (brl->w == NULL) {
		talloc_free(brl);
		return NULL;
	}

	brl->server = server;
	brl->messaging_ctx = messaging_ctx;

	return brl;
}

struct brl_handle *brl_create_handle(TALLOC_CTX *mem_ctx, struct ntvfs_handle *ntvfs, DATA_BLOB *file_key)
{
	struct brl_handle *brlh;

	brlh = talloc(mem_ctx, struct brl_handle);
	if (brlh == NULL) {
		return NULL;
	}

	brlh->key = *file_key;
	brlh->ntvfs = ntvfs;
	ZERO_STRUCT(brlh->last_lock);

	return brlh;
}

/*
  see if two locking contexts are equal
*/
static BOOL brl_same_context(struct lock_context *ctx1, struct lock_context *ctx2)
{
	return (ctx1->server == ctx2->server &&
		ctx1->smbpid == ctx2->smbpid &&
		ctx1->ctx == ctx2->ctx);
}

/*
  see if lck1 and lck2 overlap
*/
static BOOL brl_overlap(struct lock_struct *lck1, 
			struct lock_struct *lck2)
{
	/* this extra check is not redundent - it copes with locks
	   that go beyond the end of 64 bit file space */
	if (lck1->size != 0 &&
	    lck1->start == lck2->start &&
	    lck1->size == lck2->size) {
		return True;
	}
	    
	if (lck1->start >= (lck2->start+lck2->size) ||
	    lck2->start >= (lck1->start+lck1->size)) {
		return False;
	}
	return True;
} 

/*
 See if lock2 can be added when lock1 is in place.
*/
static BOOL brl_conflict(struct lock_struct *lck1, 
			 struct lock_struct *lck2)
{
	/* pending locks don't conflict with anything */
	if (lck1->lock_type >= PENDING_READ_LOCK ||
	    lck2->lock_type >= PENDING_READ_LOCK) {
		return False;
	}

	if (lck1->lock_type == READ_LOCK && lck2->lock_type == READ_LOCK) {
		return False;
	}

	if (brl_same_context(&lck1->context, &lck2->context) &&
	    lck2->lock_type == READ_LOCK && lck1->ntvfs == lck2->ntvfs) {
		return False;
	}

	return brl_overlap(lck1, lck2);
} 


/*
 Check to see if this lock conflicts, but ignore our own locks on the
 same fnum only.
*/
static BOOL brl_conflict_other(struct lock_struct *lck1, struct lock_struct *lck2)
{
	/* pending locks don't conflict with anything */
	if (lck1->lock_type >= PENDING_READ_LOCK ||
	    lck2->lock_type >= PENDING_READ_LOCK) {
		return False;
	}

	if (lck1->lock_type == READ_LOCK && lck2->lock_type == READ_LOCK) 
		return False;

	/*
	 * note that incoming write calls conflict with existing READ
	 * locks even if the context is the same. JRA. See LOCKTEST7
	 * in smbtorture.
	 */
	if (brl_same_context(&lck1->context, &lck2->context) &&
	    lck1->ntvfs == lck2->ntvfs &&
	    (lck2->lock_type == READ_LOCK || lck1->lock_type == WRITE_LOCK)) {
		return False;
	}

	return brl_overlap(lck1, lck2);
} 


/*
  amazingly enough, w2k3 "remembers" whether the last lock failure
  is the same as this one and changes its error code. I wonder if any
  app depends on this?
*/
static NTSTATUS brl_lock_failed(struct brl_handle *brlh, struct lock_struct *lock)
{
	/*
	 * this function is only called for non pending lock!
	 */

	/* 
	 * if the notify_ptr is non NULL,
	 * it means that we're at the end of a pending lock
	 * and the real lock is requested after the timout went by
	 * In this case we need to remember the last_lock and always
	 * give FILE_LOCK_CONFLICT
	 */
	if (lock->notify_ptr) {
		brlh->last_lock = *lock;
		return NT_STATUS_FILE_LOCK_CONFLICT;
	}

	/* 
	 * amazing the little things you learn with a test
	 * suite. Locks beyond this offset (as a 64 bit
	 * number!) always generate the conflict error code,
	 * unless the top bit is set
	 */
	if (lock->start >= 0xEF000000 && (lock->start >> 63) == 0) {
		brlh->last_lock = *lock;
		return NT_STATUS_FILE_LOCK_CONFLICT;
	}

	/*
	 * if the current lock matches the last failed lock on the file handle
	 * and starts at the same offset, then FILE_LOCK_CONFLICT should be returned
	 */
	if (lock->context.server == brlh->last_lock.context.server &&
	    lock->context.ctx == brlh->last_lock.context.ctx &&
	    lock->ntvfs == brlh->last_lock.ntvfs &&
	    lock->start == brlh->last_lock.start) {
		return NT_STATUS_FILE_LOCK_CONFLICT;
	}

	brlh->last_lock = *lock;
	return NT_STATUS_LOCK_NOT_GRANTED;
}

/*
  Lock a range of bytes.  The lock_type can be a PENDING_*_LOCK, in
  which case a real lock is first tried, and if that fails then a
  pending lock is created. When the pending lock is triggered (by
  someone else closing an overlapping lock range) a messaging
  notification is sent, identified by the notify_ptr
*/
NTSTATUS brl_lock(struct brl_context *brl,
		  struct brl_handle *brlh,
		  uint16_t smbpid,
		  uint64_t start, uint64_t size, 
		  enum brl_type lock_type,
		  void *notify_ptr)
{
	TDB_DATA kbuf, dbuf;
	int count=0, i;
	struct lock_struct lock, *locks=NULL;
	NTSTATUS status;

	kbuf.dptr = brlh->key.data;
	kbuf.dsize = brlh->key.length;

	if (tdb_chainlock(brl->w->tdb, kbuf) != 0) {
		return NT_STATUS_INTERNAL_DB_CORRUPTION;
	}

	/* if this is a pending lock, then with the chainlock held we
	   try to get the real lock. If we succeed then we don't need
	   to make it pending. This prevents a possible race condition
	   where the pending lock gets created after the lock that is
	   preventing the real lock gets removed */
	if (lock_type >= PENDING_READ_LOCK) {
		enum brl_type rw = (lock_type==PENDING_READ_LOCK? READ_LOCK : WRITE_LOCK);

		/* here we need to force that the last_lock isn't overwritten */
		lock = brlh->last_lock;
		status = brl_lock(brl, brlh, smbpid, start, size, rw, NULL);
		brlh->last_lock = lock;

		if (NT_STATUS_IS_OK(status)) {
			tdb_chainunlock(brl->w->tdb, kbuf);
			return NT_STATUS_OK;
		}
	}

	dbuf = tdb_fetch(brl->w->tdb, kbuf);

	lock.context.smbpid = smbpid;
	lock.context.server = brl->server;
	lock.context.ctx = brl;
	lock.ntvfs = brlh->ntvfs;
	lock.context.ctx = brl;
	lock.start = start;
	lock.size = size;
	lock.lock_type = lock_type;
	lock.notify_ptr = notify_ptr;

	if (dbuf.dptr) {
		/* there are existing locks - make sure they don't conflict */
		locks = (struct lock_struct *)dbuf.dptr;
		count = dbuf.dsize / sizeof(*locks);
		for (i=0; i<count; i++) {
			if (brl_conflict(&locks[i], &lock)) {
				status = brl_lock_failed(brlh, &lock);
				goto fail;
			}
		}
	}

	/* no conflicts - add it to the list of locks */
	locks = realloc_p(locks, struct lock_struct, count+1);
	if (!locks) {
		status = NT_STATUS_NO_MEMORY;
		goto fail;
	} else {
		dbuf.dptr = (uint8_t *)locks;
	}
	locks[count] = lock;
	dbuf.dsize += sizeof(lock);

	if (tdb_store(brl->w->tdb, kbuf, dbuf, TDB_REPLACE) != 0) {
		status = NT_STATUS_INTERNAL_DB_CORRUPTION;
		goto fail;
	}

	free(dbuf.dptr);
	tdb_chainunlock(brl->w->tdb, kbuf);

	/* the caller needs to know if the real lock was granted. If
	   we have reached here then it must be a pending lock that
	   was granted, so tell them the lock failed */
	if (lock_type >= PENDING_READ_LOCK) {
		return NT_STATUS_LOCK_NOT_GRANTED;
	}

	return NT_STATUS_OK;

 fail:

	free(dbuf.dptr);
	tdb_chainunlock(brl->w->tdb, kbuf);
	return status;
}


/*
  we are removing a lock that might be holding up a pending lock. Scan for pending
  locks that cover this range and if we find any then notify the server that it should
  retry the lock
*/
static void brl_notify_unlock(struct brl_context *brl,
			      struct lock_struct *locks, int count, 
			      struct lock_struct *removed_lock)
{
	int i, last_notice;

	/* the last_notice logic is to prevent stampeding on a lock
	   range. It prevents us sending hundreds of notifies on the
	   same range of bytes. It doesn't prevent all possible
	   stampedes, but it does prevent the most common problem */
	last_notice = -1;

	for (i=0;i<count;i++) {
		if (locks[i].lock_type >= PENDING_READ_LOCK &&
		    brl_overlap(&locks[i], removed_lock)) {
			if (last_notice != -1 && brl_overlap(&locks[i], &locks[last_notice])) {
				continue;
			}
			if (locks[i].lock_type == PENDING_WRITE_LOCK) {
				last_notice = i;
			}
			messaging_send_ptr(brl->messaging_ctx, locks[i].context.server, 
					   MSG_BRL_RETRY, locks[i].notify_ptr);
		}
	}
}


/*
  send notifications for all pending locks - the file is being closed by this
  user
*/
static void brl_notify_all(struct brl_context *brl,
			   struct lock_struct *locks, int count)
{
	int i;
	for (i=0;i<count;i++) {
		if (locks->lock_type >= PENDING_READ_LOCK) {
			brl_notify_unlock(brl, locks, count, &locks[i]);
		}
	}
}



/*
 Unlock a range of bytes.
*/
NTSTATUS brl_unlock(struct brl_context *brl,
		    struct brl_handle *brlh, 
		    uint16_t smbpid,
		    uint64_t start, uint64_t size)
{
	TDB_DATA kbuf, dbuf;
	int count, i;
	struct lock_struct *locks;
	struct lock_context context;
	NTSTATUS status;

	kbuf.dptr = brlh->key.data;
	kbuf.dsize = brlh->key.length;

	if (tdb_chainlock(brl->w->tdb, kbuf) != 0) {
		return NT_STATUS_INTERNAL_DB_CORRUPTION;
	}

	dbuf = tdb_fetch(brl->w->tdb, kbuf);
	if (!dbuf.dptr) {
		tdb_chainunlock(brl->w->tdb, kbuf);
		return NT_STATUS_RANGE_NOT_LOCKED;
	}

	context.smbpid = smbpid;
	context.server = brl->server;
	context.ctx = brl;

	/* there are existing locks - find a match */
	locks = (struct lock_struct *)dbuf.dptr;
	count = dbuf.dsize / sizeof(*locks);

	for (i=0; i<count; i++) {
		struct lock_struct *lock = &locks[i];
		
		if (brl_same_context(&lock->context, &context) &&
		    lock->ntvfs == brlh->ntvfs &&
		    lock->start == start &&
		    lock->size == size &&
		    lock->lock_type < PENDING_READ_LOCK) {
			/* found it - delete it */
			if (count == 1) {
				if (tdb_delete(brl->w->tdb, kbuf) != 0) {
					status = NT_STATUS_INTERNAL_DB_CORRUPTION;
					goto fail;
				}
			} else {
				struct lock_struct removed_lock = *lock;
				if (i < count-1) {
					memmove(&locks[i], &locks[i+1], 
						sizeof(*locks)*((count-1) - i));
				}
				count--;

				/* send notifications for any relevant pending locks */
				brl_notify_unlock(brl, locks, count, &removed_lock);

				dbuf.dsize = count * sizeof(*locks);

				if (tdb_store(brl->w->tdb, kbuf, dbuf, TDB_REPLACE) != 0) {
					status = NT_STATUS_INTERNAL_DB_CORRUPTION;
					goto fail;
				}
			}
			
			free(dbuf.dptr);
			tdb_chainunlock(brl->w->tdb, kbuf);
			return NT_STATUS_OK;
		}
	}
	
	/* we didn't find it */
	status = NT_STATUS_RANGE_NOT_LOCKED;

 fail:
	free(dbuf.dptr);
	tdb_chainunlock(brl->w->tdb, kbuf);
	return status;
}


/*
  remove a pending lock. This is called when the caller has either
  given up trying to establish a lock or when they have succeeded in
  getting it. In either case they no longer need to be notified.
*/
NTSTATUS brl_remove_pending(struct brl_context *brl,
			    struct brl_handle *brlh, 
			    void *notify_ptr)
{
	TDB_DATA kbuf, dbuf;
	int count, i;
	struct lock_struct *locks;
	NTSTATUS status;

	kbuf.dptr = brlh->key.data;
	kbuf.dsize = brlh->key.length;

	if (tdb_chainlock(brl->w->tdb, kbuf) != 0) {
		return NT_STATUS_INTERNAL_DB_CORRUPTION;
	}

	dbuf = tdb_fetch(brl->w->tdb, kbuf);
	if (!dbuf.dptr) {
		tdb_chainunlock(brl->w->tdb, kbuf);
		return NT_STATUS_RANGE_NOT_LOCKED;
	}

	/* there are existing locks - find a match */
	locks = (struct lock_struct *)dbuf.dptr;
	count = dbuf.dsize / sizeof(*locks);

	for (i=0; i<count; i++) {
		struct lock_struct *lock = &locks[i];
		
		if (lock->lock_type >= PENDING_READ_LOCK &&
		    lock->notify_ptr == notify_ptr &&
		    lock->context.server == brl->server) {
			/* found it - delete it */
			if (count == 1) {
				if (tdb_delete(brl->w->tdb, kbuf) != 0) {
					status = NT_STATUS_INTERNAL_DB_CORRUPTION;
					goto fail;
				}
			} else {
				if (i < count-1) {
					memmove(&locks[i], &locks[i+1], 
						sizeof(*locks)*((count-1) - i));
				}
				count--;
				dbuf.dsize = count * sizeof(*locks);
				if (tdb_store(brl->w->tdb, kbuf, dbuf, TDB_REPLACE) != 0) {
					status = NT_STATUS_INTERNAL_DB_CORRUPTION;
					goto fail;
				}
			}
			
			free(dbuf.dptr);
			tdb_chainunlock(brl->w->tdb, kbuf);
			return NT_STATUS_OK;
		}
	}
	
	/* we didn't find it */
	status = NT_STATUS_RANGE_NOT_LOCKED;

 fail:
	free(dbuf.dptr);
	tdb_chainunlock(brl->w->tdb, kbuf);
	return status;
}


/*
  Test if we are allowed to perform IO on a region of an open file
*/
NTSTATUS brl_locktest(struct brl_context *brl,
		      struct brl_handle *brlh,
		      uint16_t smbpid, 
		      uint64_t start, uint64_t size, 
		      enum brl_type lock_type)
{
	TDB_DATA kbuf, dbuf;
	int count, i;
	struct lock_struct lock, *locks;

	kbuf.dptr = brlh->key.data;
	kbuf.dsize = brlh->key.length;

	dbuf = tdb_fetch(brl->w->tdb, kbuf);
	if (dbuf.dptr == NULL) {
		return NT_STATUS_OK;
	}

	lock.context.smbpid = smbpid;
	lock.context.server = brl->server;
	lock.context.ctx = brl;
	lock.ntvfs = brlh->ntvfs;
	lock.start = start;
	lock.size = size;
	lock.lock_type = lock_type;

	/* there are existing locks - make sure they don't conflict */
	locks = (struct lock_struct *)dbuf.dptr;
	count = dbuf.dsize / sizeof(*locks);

	for (i=0; i<count; i++) {
		if (brl_conflict_other(&locks[i], &lock)) {
			free(dbuf.dptr);
			return NT_STATUS_FILE_LOCK_CONFLICT;
		}
	}

	free(dbuf.dptr);
	return NT_STATUS_OK;
}


/*
 Remove any locks associated with a open file.
*/
NTSTATUS brl_close(struct brl_context *brl,
		   struct brl_handle *brlh)
{
	TDB_DATA kbuf, dbuf;
	int count, i, dcount=0;
	struct lock_struct *locks;
	NTSTATUS status;

	kbuf.dptr = brlh->key.data;
	kbuf.dsize = brlh->key.length;

	if (tdb_chainlock(brl->w->tdb, kbuf) != 0) {
		return NT_STATUS_INTERNAL_DB_CORRUPTION;
	}

	dbuf = tdb_fetch(brl->w->tdb, kbuf);
	if (!dbuf.dptr) {
		tdb_chainunlock(brl->w->tdb, kbuf);
		return NT_STATUS_OK;
	}

	/* there are existing locks - remove any for this fnum */
	locks = (struct lock_struct *)dbuf.dptr;
	count = dbuf.dsize / sizeof(*locks);

	for (i=0; i<count; i++) {
		struct lock_struct *lock = &locks[i];

		if (lock->context.ctx == brl &&
		    lock->context.server == brl->server &&
		    lock->ntvfs == brlh->ntvfs) {
			/* found it - delete it */
			if (count > 1 && i < count-1) {
				memmove(&locks[i], &locks[i+1], 
					sizeof(*locks)*((count-1) - i));
			}
			count--;
			i--;
			dcount++;
		}
	}

	status = NT_STATUS_OK;

	if (count == 0) {
		if (tdb_delete(brl->w->tdb, kbuf) != 0) {
			status = NT_STATUS_INTERNAL_DB_CORRUPTION;
		}
	} else if (dcount != 0) {
		/* tell all pending lock holders for this file that
		   they have a chance now. This is a bit indiscriminant,
		   but works OK */
		brl_notify_all(brl, locks, count);

		dbuf.dsize = count * sizeof(*locks);

		if (tdb_store(brl->w->tdb, kbuf, dbuf, TDB_REPLACE) != 0) {
			status = NT_STATUS_INTERNAL_DB_CORRUPTION;
		}
	}

	free(dbuf.dptr);
	tdb_chainunlock(brl->w->tdb, kbuf);

	return status;
}

