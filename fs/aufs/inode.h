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

struct au_hinode {
	struct inode		*hi_inode;
};

struct au_iinfo {
	struct rw_semaphore	ii_rwsem;
	aufs_bindex_t		ii_bstart, ii_bend;
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

void au_icntnr_init_once(void *_c);
int au_iinfo_init(struct inode *inode);
void au_iinfo_fin(struct inode *inode);

/* ---------------------------------------------------------------------- */

static inline void au_icntnr_init(struct au_icntnr *c)
{
#ifdef CONFIG_AUFS_DEBUG
	c->vfs_inode.i_mode = 0;
#endif
}

/* ---------------------------------------------------------------------- */

static inline aufs_bindex_t au_ibstart(struct inode *inode)
{
	return au_ii(inode)->ii_bstart;
}

static inline aufs_bindex_t au_ibend(struct inode *inode)
{
	return au_ii(inode)->ii_bend;
}

static inline void au_set_ibstart(struct inode *inode, aufs_bindex_t bindex)
{
	au_ii(inode)->ii_bstart = bindex;
}

static inline void au_set_ibend(struct inode *inode, aufs_bindex_t bindex)
{
	au_ii(inode)->ii_bend = bindex;
}

static inline struct au_hinode *au_hi(struct inode *inode, aufs_bindex_t bindex)
{
	return au_ii(inode)->ii_hinode + bindex;
}

#endif /* __KERNEL__ */
#endif /* __AUFS_INODE_H__ */
