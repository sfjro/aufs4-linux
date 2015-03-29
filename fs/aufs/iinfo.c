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

	IiMustAnyLock(inode);

	h_inode = au_ii(inode)->ii_hinode[0 + bindex].hi_inode;
	AuDebugOn(h_inode && atomic_read(&h_inode->i_count) <= 0);
	return h_inode;
}

/* todo: hard/soft set? */
void au_hiput(struct au_hinode *hinode)
{
	iput(hinode->hi_inode);
}

unsigned int au_hi_flags(struct inode *inode, int isdir)
{
	unsigned int flags;
	const unsigned int mnt_flags = au_mntflags(inode->i_sb);

	flags = 0;
	if (au_opt_test(mnt_flags, XINO))
		au_fset_hi(flags, XINO);
	return flags;
}

void au_set_h_iptr(struct inode *inode, aufs_bindex_t bindex,
		   struct inode *h_inode, unsigned int flags)
{
	struct au_hinode *hinode;
	struct inode *hi;
	struct au_iinfo *iinfo = au_ii(inode);

	IiMustWriteLock(inode);

	hinode = iinfo->ii_hinode + bindex;
	hi = hinode->hi_inode;
	AuDebugOn(h_inode && atomic_read(&h_inode->i_count) <= 0);

	if (hi)
		au_hiput(hinode);
	hinode->hi_inode = h_inode;
	if (h_inode) {
		int err;
		struct super_block *sb = inode->i_sb;
		struct au_branch *br;

		AuDebugOn(inode->i_mode
			  && (h_inode->i_mode & S_IFMT)
			  != (inode->i_mode & S_IFMT));
		if (bindex == iinfo->ii_bstart)
			au_cpup_igen(inode, h_inode);
		br = au_sbr(sb, bindex);
		hinode->hi_id = br->br_id;
		if (au_ftest_hi(flags, XINO)) {
			err = au_xino_write(sb, bindex, h_inode->i_ino,
					    inode->i_ino);
			if (unlikely(err))
				AuIOErr1("failed au_xino_write() %d\n", err);
		}
	}
}

void au_update_iigen(struct inode *inode, int half)
{
	struct au_iinfo *iinfo;
	struct au_iigen *iigen;
	unsigned int sigen;

	sigen = au_sigen(inode->i_sb);
	iinfo = au_ii(inode);
	iigen = &iinfo->ii_generation;
	spin_lock(&iinfo->ii_genspin);
	iigen->ig_generation = sigen;
	spin_unlock(&iinfo->ii_genspin);
}

/* ---------------------------------------------------------------------- */

void au_icntnr_init_once(void *_c)
{
	struct au_icntnr *c = _c;
	struct au_iinfo *iinfo = &c->iinfo;
	static struct lock_class_key aufs_ii;

	spin_lock_init(&iinfo->ii_genspin);
	au_rw_init(&iinfo->ii_rwsem);
	au_rw_class(&iinfo->ii_rwsem, &aufs_ii);
	inode_init_once(&c->vfs_inode);
}

int au_iinfo_init(struct inode *inode)
{
	struct au_iinfo *iinfo;
	struct super_block *sb;
	int nbr, i;

	sb = inode->i_sb;
	iinfo = &(container_of(inode, struct au_icntnr, vfs_inode)->iinfo);
	nbr = au_sbend(sb) + 1;
	if (unlikely(nbr <= 0))
		nbr = 1;
	iinfo->ii_hinode = kcalloc(nbr, sizeof(*iinfo->ii_hinode), GFP_NOFS);
	if (iinfo->ii_hinode) {
		for (i = 0; i < nbr; i++)
			iinfo->ii_hinode[i].hi_id = -1;

		iinfo->ii_generation.ig_generation = au_sigen(sb);
		iinfo->ii_bstart = -1;
		iinfo->ii_bend = -1;
		iinfo->ii_vdir = NULL;
		return 0;
	}
	return -ENOMEM;
}

int au_ii_realloc(struct au_iinfo *iinfo, int nbr)
{
	int err, sz;
	struct au_hinode *hip;

	AuRwMustWriteLock(&iinfo->ii_rwsem);

	err = -ENOMEM;
	sz = sizeof(*hip) * (iinfo->ii_bend + 1);
	if (!sz)
		sz = sizeof(*hip);
	hip = au_kzrealloc(iinfo->ii_hinode, sz, sizeof(*hip) * nbr, GFP_NOFS);
	if (hip) {
		iinfo->ii_hinode = hip;
		err = 0;
	}

	return err;
}

void au_iinfo_fin(struct inode *inode)
{
	struct au_iinfo *iinfo;
	struct au_hinode *hi;
	struct super_block *sb;
	aufs_bindex_t bindex, bend;
	const unsigned char unlinked = !inode->i_nlink;

	iinfo = au_ii(inode);
	/* bad_inode case */
	if (!iinfo)
		return;

	sb = inode->i_sb;
	if (si_pid_test(sb))
		au_xino_delete_inode(inode, unlinked);
	else {
		/*
		 * it is safe to hide the dependency between sbinfo and
		 * sb->s_umount.
		 */
		lockdep_off();
		si_noflush_read_lock(sb);
		au_xino_delete_inode(inode, unlinked);
		si_read_unlock(sb);
		lockdep_on();
	}

	if (iinfo->ii_vdir)
		au_vdir_free(iinfo->ii_vdir);

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
	AuRwDestroy(&iinfo->ii_rwsem);
}
