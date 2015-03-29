/*
 * Copyright (C) 2005-2015 Junjiro R. Okajima
 *
 * This program, aufs is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * inode operations
 */

#ifndef __AUFS_INODE_H__
#define __AUFS_INODE_H__

#ifdef __KERNEL__

#include <linux/fs.h>
#include "rwsem.h"

struct au_hinode {
	struct inode		*hi_inode;
};

struct au_iigen {
	__u32		ig_generation;
};

struct au_iinfo {
	spinlock_t		ii_genspin;
	struct au_iigen		ii_generation;
	struct super_block	*ii_hsb1;	/* no get/put */

	struct au_rwsem		ii_rwsem;
	aufs_bindex_t		ii_bstart, ii_bend;
	__u32			ii_higen;
	struct au_hinode	*ii_hinode;
};

struct au_icntnr {
	struct au_iinfo iinfo;
	struct inode vfs_inode;
} ____cacheline_aligned_in_smp;

/* ---------------------------------------------------------------------- */

static inline struct au_iinfo *au_ii(struct inode *inode)
{
	struct au_iinfo *iinfo;

	iinfo = &(container_of(inode, struct au_icntnr, vfs_inode)->iinfo);
	if (iinfo->ii_hinode)
		return iinfo;
	return NULL; /* debugging bad_inode case */
}

/* ---------------------------------------------------------------------- */

/* iinfo.c */
struct inode *au_h_iptr(struct inode *inode, aufs_bindex_t bindex);
void au_hiput(struct au_hinode *hinode);

void au_update_iigen(struct inode *inode, int half);

void au_icntnr_init_once(void *_c);
int au_iinfo_init(struct inode *inode);
void au_iinfo_fin(struct inode *inode);

/* ---------------------------------------------------------------------- */

/* lock subclass for iinfo */
enum {
	AuLsc_II_CHILD,		/* child first */
	AuLsc_II_CHILD2,	/* rename(2), link(2), and cpup at hnotify */
	AuLsc_II_CHILD3,	/* copyup dirs */
	AuLsc_II_PARENT,	/* see AuLsc_I_PARENT in vfsub.h */
	AuLsc_II_PARENT2,
	AuLsc_II_PARENT3,	/* copyup dirs */
	AuLsc_II_NEW_CHILD
};

/*
 * ii_read_lock_child, ii_write_lock_child,
 * ii_read_lock_child2, ii_write_lock_child2,
 * ii_read_lock_child3, ii_write_lock_child3,
 * ii_read_lock_parent, ii_write_lock_parent,
 * ii_read_lock_parent2, ii_write_lock_parent2,
 * ii_read_lock_parent3, ii_write_lock_parent3,
 * ii_read_lock_new_child, ii_write_lock_new_child,
 */
#define AuReadLockFunc(name, lsc) \
static inline void ii_read_lock_##name(struct inode *i) \
{ \
	au_rw_read_lock_nested(&au_ii(i)->ii_rwsem, AuLsc_II_##lsc); \
}

#define AuWriteLockFunc(name, lsc) \
static inline void ii_write_lock_##name(struct inode *i) \
{ \
	au_rw_write_lock_nested(&au_ii(i)->ii_rwsem, AuLsc_II_##lsc); \
}

#define AuRWLockFuncs(name, lsc) \
	AuReadLockFunc(name, lsc) \
	AuWriteLockFunc(name, lsc)

AuRWLockFuncs(child, CHILD);
AuRWLockFuncs(child2, CHILD2);
AuRWLockFuncs(child3, CHILD3);
AuRWLockFuncs(parent, PARENT);
AuRWLockFuncs(parent2, PARENT2);
AuRWLockFuncs(parent3, PARENT3);
AuRWLockFuncs(new_child, NEW_CHILD);

#undef AuReadLockFunc
#undef AuWriteLockFunc
#undef AuRWLockFuncs

/*
 * ii_read_unlock, ii_write_unlock, ii_downgrade_lock
 */
AuSimpleUnlockRwsemFuncs(ii, struct inode *i, &au_ii(i)->ii_rwsem);

#define IiMustNoWaiters(i)	AuRwMustNoWaiters(&au_ii(i)->ii_rwsem)
#define IiMustAnyLock(i)	AuRwMustAnyLock(&au_ii(i)->ii_rwsem)
#define IiMustWriteLock(i)	AuRwMustWriteLock(&au_ii(i)->ii_rwsem)

/* ---------------------------------------------------------------------- */

static inline void au_icntnr_init(struct au_icntnr *c)
{
#ifdef CONFIG_AUFS_DEBUG
	c->vfs_inode.i_mode = 0;
#endif
}

static inline unsigned int au_iigen(struct inode *inode, struct au_iigen *iigen)
{
	unsigned int gen;
	struct au_iinfo *iinfo;

	iinfo = au_ii(inode);
	spin_lock(&iinfo->ii_genspin);
	if (iigen)
		*iigen = iinfo->ii_generation;
	gen = iinfo->ii_generation.ig_generation;
	spin_unlock(&iinfo->ii_genspin);

	return gen;
}

/* tiny test for inode number */
/* tmpfs generation is too rough */
static inline int au_test_higen(struct inode *inode, struct inode *h_inode)
{
	struct au_iinfo *iinfo;

	iinfo = au_ii(inode);
	AuRwMustAnyLock(&iinfo->ii_rwsem);
	return !(iinfo->ii_hsb1 == h_inode->i_sb
		 && iinfo->ii_higen == h_inode->i_generation);
}

static inline void au_iigen_dec(struct inode *inode)
{
	struct au_iinfo *iinfo;

	iinfo = au_ii(inode);
	spin_lock(&iinfo->ii_genspin);
	iinfo->ii_generation.ig_generation--;
	spin_unlock(&iinfo->ii_genspin);
}

static inline int au_iigen_test(struct inode *inode, unsigned int sigen)
{
	int err;

	err = 0;
	if (unlikely(inode && au_iigen(inode, NULL) != sigen))
		err = -EIO;

	return err;
}

/* ---------------------------------------------------------------------- */

static inline aufs_bindex_t au_ibstart(struct inode *inode)
{
	IiMustAnyLock(inode);
	return au_ii(inode)->ii_bstart;
}

static inline aufs_bindex_t au_ibend(struct inode *inode)
{
	IiMustAnyLock(inode);
	return au_ii(inode)->ii_bend;
}

static inline void au_set_ibstart(struct inode *inode, aufs_bindex_t bindex)
{
	IiMustWriteLock(inode);
	au_ii(inode)->ii_bstart = bindex;
}

static inline void au_set_ibend(struct inode *inode, aufs_bindex_t bindex)
{
	IiMustWriteLock(inode);
	au_ii(inode)->ii_bend = bindex;
}

static inline struct au_hinode *au_hi(struct inode *inode, aufs_bindex_t bindex)
{
	IiMustAnyLock(inode);
	return au_ii(inode)->ii_hinode + bindex;
}

#endif /* __KERNEL__ */
#endif /* __AUFS_INODE_H__ */
