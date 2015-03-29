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
 * inode private data
 */

#include "aufs.h"

struct inode *au_h_iptr(struct inode *inode, aufs_bindex_t bindex)
{
	struct inode *h_inode;

	h_inode = au_ii(inode)->ii_hinode[0 + bindex].hi_inode;
	return h_inode;
}

/* todo: hard/soft set? */
void au_hiput(struct au_hinode *hinode)
{
	iput(hinode->hi_inode);
}

/* ---------------------------------------------------------------------- */

void au_icntnr_init_once(void *_c)
{
	struct au_icntnr *c = _c;
	struct au_iinfo *iinfo = &c->iinfo;
	static struct lock_class_key aufs_ii;

	init_rwsem(&iinfo->ii_rwsem);
	lockdep_set_class(&iinfo->ii_rwsem, &aufs_ii);
	down_write(&iinfo->ii_rwsem);
	inode_init_once(&c->vfs_inode);
}

int au_iinfo_init(struct inode *inode)
{
	struct au_iinfo *iinfo;
	struct super_block *sb;
	int nbr;

	sb = inode->i_sb;
	iinfo = &(container_of(inode, struct au_icntnr, vfs_inode)->iinfo);
	nbr = 1; /* re-commit later */
	iinfo->ii_hinode = kcalloc(nbr, sizeof(*iinfo->ii_hinode), GFP_NOFS);
	if (iinfo->ii_hinode) {
		iinfo->ii_bstart = -1;
		iinfo->ii_bend = -1;
		return 0;
	}
	return -ENOMEM;
}

void au_iinfo_fin(struct inode *inode)
{
	struct au_iinfo *iinfo;
	struct au_hinode *hi;
	aufs_bindex_t bindex, bend;

	iinfo = au_ii(inode);
	/* bad_inode case */
	if (!iinfo)
		return;

	bindex = iinfo->ii_bstart;
	if (bindex >= 0) {
		hi = iinfo->ii_hinode + bindex;
		bend = iinfo->ii_bend;
		while (bindex++ <= bend) {
			if (hi->hi_inode)
				au_hiput(hi);
			hi++;
		}
	}
	kfree(iinfo->ii_hinode);
	iinfo->ii_hinode = NULL;
}
