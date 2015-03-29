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
 * policies for selecting one among multiple writable branches
 */

#include "aufs.h"

/* subset of cpup_attr() */
static noinline_for_stack
int au_cpdown_attr(struct path *h_path, struct dentry *h_src)
{
	int err, sbits;
	struct iattr ia;
	struct inode *h_isrc;

	h_isrc = h_src->d_inode;
	ia.ia_valid = ATTR_FORCE | ATTR_MODE | ATTR_UID | ATTR_GID;
	ia.ia_mode = h_isrc->i_mode;
	ia.ia_uid = h_isrc->i_uid;
	ia.ia_gid = h_isrc->i_gid;
	sbits = !!(ia.ia_mode & (S_ISUID | S_ISGID));
	au_cpup_attr_flags(h_path->dentry->d_inode, h_isrc->i_flags);
	/* no delegation since it is just created */
	err = vfsub_sio_notify_change(h_path, &ia, /*delegated*/NULL);

	/* is this nfs only? */
	if (!err && sbits && au_test_nfs(h_path->dentry->d_sb)) {
		ia.ia_valid = ATTR_FORCE | ATTR_MODE;
		ia.ia_mode = h_isrc->i_mode;
		err = vfsub_sio_notify_change(h_path, &ia, /*delegated*/NULL);
	}

	return err;
}

#define AuCpdown_PARENT_OPQ	1
#define AuCpdown_WHED		(1 << 1)
#define AuCpdown_MADE_DIR	(1 << 2)
#define AuCpdown_DIROPQ		(1 << 3)
#define au_ftest_cpdown(flags, name)	((flags) & AuCpdown_##name)
#define au_fset_cpdown(flags, name) \
	do { (flags) |= AuCpdown_##name; } while (0)
#define au_fclr_cpdown(flags, name) \
	do { (flags) &= ~AuCpdown_##name; } while (0)

static int au_cpdown_dir_opq(struct dentry *dentry, aufs_bindex_t bdst,
			     unsigned int *flags)
{
	int err;
	struct dentry *opq_dentry;

	opq_dentry = au_diropq_create(dentry, bdst);
	err = PTR_ERR(opq_dentry);
	if (IS_ERR(opq_dentry))
		goto out;
	dput(opq_dentry);
	au_fset_cpdown(*flags, DIROPQ);

out:
	return err;
}

static int au_cpdown_dir_wh(struct dentry *dentry, struct dentry *h_parent,
			    struct inode *dir, aufs_bindex_t bdst)
{
	int err;
	struct path h_path;
	struct au_branch *br;

	br = au_sbr(dentry->d_sb, bdst);
	h_path.dentry = au_wh_lkup(h_parent, &dentry->d_name, br);
	err = PTR_ERR(h_path.dentry);
	if (IS_ERR(h_path.dentry))
		goto out;

	err = 0;
	if (h_path.dentry->d_inode) {
		h_path.mnt = au_br_mnt(br);
		err = au_wh_unlink_dentry(au_h_iptr(dir, bdst), &h_path,
					  dentry);
	}
	dput(h_path.dentry);

out:
	return err;
}

static int au_cpdown_dir(struct dentry *dentry, aufs_bindex_t bdst,
			 struct au_pin *pin,
			 struct dentry *h_parent, void *arg)
{
	int err, rerr;
	aufs_bindex_t bopq, bstart;
	struct path h_path;
	struct dentry *parent;
	struct inode *h_dir, *h_inode, *inode, *dir;
	unsigned int *flags = arg;

	bstart = au_dbstart(dentry);
	/* dentry is di-locked */
	parent = dget_parent(dentry);
	dir = parent->d_inode;
	h_dir = h_parent->d_inode;
	AuDebugOn(h_dir != au_h_iptr(dir, bdst));
	IMustLock(h_dir);

	err = au_lkup_neg(dentry, bdst, /*wh*/0);
	if (unlikely(err < 0))
		goto out;
	h_path.dentry = au_h_dptr(dentry, bdst);
	h_path.mnt = au_sbr_mnt(dentry->d_sb, bdst);
	err = vfsub_sio_mkdir(au_h_iptr(dir, bdst), &h_path,
			      S_IRWXU | S_IRUGO | S_IXUGO);
	if (unlikely(err))
		goto out_put;
	au_fset_cpdown(*flags, MADE_DIR);

	bopq = au_dbdiropq(dentry);
	au_fclr_cpdown(*flags, WHED);
	au_fclr_cpdown(*flags, DIROPQ);
	if (au_dbwh(dentry) == bdst)
		au_fset_cpdown(*flags, WHED);
	if (!au_ftest_cpdown(*flags, PARENT_OPQ) && bopq <= bdst)
		au_fset_cpdown(*flags, PARENT_OPQ);
	h_inode = h_path.dentry->d_inode;
	mutex_lock_nested(&h_inode->i_mutex, AuLsc_I_CHILD);
	if (au_ftest_cpdown(*flags, WHED)) {
		err = au_cpdown_dir_opq(dentry, bdst, flags);
		if (unlikely(err)) {
			mutex_unlock(&h_inode->i_mutex);
			goto out_dir;
		}
	}

	err = au_cpdown_attr(&h_path, au_h_dptr(dentry, bstart));
	mutex_unlock(&h_inode->i_mutex);
	if (unlikely(err))
		goto out_opq;

	if (au_ftest_cpdown(*flags, WHED)) {
		err = au_cpdown_dir_wh(dentry, h_parent, dir, bdst);
		if (unlikely(err))
			goto out_opq;
	}

	inode = dentry->d_inode;
	if (au_ibend(inode) < bdst)
		au_set_ibend(inode, bdst);
	au_set_h_iptr(inode, bdst, au_igrab(h_inode),
		      au_hi_flags(inode, /*isdir*/1));
	goto out; /* success */

	/* revert */
out_opq:
	if (au_ftest_cpdown(*flags, DIROPQ)) {
		mutex_lock_nested(&h_inode->i_mutex, AuLsc_I_CHILD);
		rerr = au_diropq_remove(dentry, bdst);
		mutex_unlock(&h_inode->i_mutex);
		if (unlikely(rerr)) {
			AuIOErr("failed removing diropq for %pd b%d (%d)\n",
				dentry, bdst, rerr);
			err = -EIO;
			goto out;
		}
	}
out_dir:
	if (au_ftest_cpdown(*flags, MADE_DIR)) {
		rerr = vfsub_sio_rmdir(au_h_iptr(dir, bdst), &h_path);
		if (unlikely(rerr)) {
			AuIOErr("failed removing %pd b%d (%d)\n",
				dentry, bdst, rerr);
			err = -EIO;
		}
	}
out_put:
	au_set_h_dptr(dentry, bdst, NULL);
	if (au_dbend(dentry) == bdst)
		au_update_dbend(dentry);
out:
	dput(parent);
	return err;
}

int au_cpdown_dirs(struct dentry *dentry, aufs_bindex_t bdst)
{
	int err;
	unsigned int flags;

	flags = 0;
	err = au_cp_dirs(dentry, bdst, au_cpdown_dir, &flags);

	return err;
}

/* ---------------------------------------------------------------------- */

/* policies for create */

int au_wbr_nonopq(struct dentry *dentry, aufs_bindex_t bindex)
{
	int err, i, j, ndentry;
	aufs_bindex_t bopq;
	struct au_dcsub_pages dpages;
	struct au_dpage *dpage;
	struct dentry **dentries, *parent, *d;

	err = au_dpages_init(&dpages, GFP_NOFS);
	if (unlikely(err))
		goto out;
	parent = dget_parent(dentry);
	err = au_dcsub_pages_rev_aufs(&dpages, parent, /*do_include*/0);
	if (unlikely(err))
		goto out_free;

	err = bindex;
	for (i = 0; i < dpages.ndpage; i++) {
		dpage = dpages.dpages + i;
		dentries = dpage->dentries;
		ndentry = dpage->ndentry;
		for (j = 0; j < ndentry; j++) {
			d = dentries[j];
			di_read_lock_parent2(d, !AuLock_IR);
			bopq = au_dbdiropq(d);
			di_read_unlock(d, !AuLock_IR);
			if (bopq >= 0 && bopq < err)
				err = bopq;
		}
	}

out_free:
	dput(parent);
	au_dpages_free(&dpages);
out:
	return err;
}

static int au_wbr_bu(struct super_block *sb, aufs_bindex_t bindex)
{
	for (; bindex >= 0; bindex--)
		if (!au_br_rdonly(au_sbr(sb, bindex)))
			return bindex;
	return -EROFS;
}

/* top down parent */
static int au_wbr_create_tdp(struct dentry *dentry,
			     unsigned int flags __maybe_unused)
{
	int err;
	aufs_bindex_t bstart, bindex;
	struct super_block *sb;
	struct dentry *parent, *h_parent;

	sb = dentry->d_sb;
	bstart = au_dbstart(dentry);
	err = bstart;
	if (!au_br_rdonly(au_sbr(sb, bstart)))
		goto out;

	err = -EROFS;
	parent = dget_parent(dentry);
	for (bindex = au_dbstart(parent); bindex < bstart; bindex++) {
		h_parent = au_h_dptr(parent, bindex);
		if (!h_parent || !h_parent->d_inode)
			continue;

		if (!au_br_rdonly(au_sbr(sb, bindex))) {
			err = bindex;
			break;
		}
	}
	dput(parent);

	/* bottom up here */
	if (unlikely(err < 0)) {
		err = au_wbr_bu(sb, bstart - 1);
		if (err >= 0)
			err = au_wbr_nonopq(dentry, err);
	}

out:
	AuDbg("b%d\n", err);
	return err;
}

/* ---------------------------------------------------------------------- */

/* policies for copyup */

/* top down parent */
static int au_wbr_copyup_tdp(struct dentry *dentry)
{
	return au_wbr_create_tdp(dentry, /*flags, anything is ok*/0);
}

/* ---------------------------------------------------------------------- */

struct au_wbr_copyup_operations au_wbr_copyup_ops[] = {
	[AuWbrCopyup_TDP] = {
		.copyup	= au_wbr_copyup_tdp
	}
};

struct au_wbr_create_operations au_wbr_create_ops[] = {
	[AuWbrCreate_TDP] = {
		.create	= au_wbr_create_tdp
	}
};
