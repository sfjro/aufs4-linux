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
 * branch management
 */

#include <linux/file.h>
#include "aufs.h"

/*
 * free a single branch
 */
static void au_br_do_free(struct au_branch *br)
{
	if (br->br_xino.xi_file)
		fput(br->br_xino.xi_file);
	mutex_destroy(&br->br_xino.xi_nondir_mtx);

	AuDebugOn(atomic_read(&br->br_count));

	/* recursive lock, s_umount of branch's */
	lockdep_off();
	path_put(&br->br_path);
	lockdep_on();
	kfree(br);
}

/*
 * frees all branches
 */
void au_br_free(struct au_sbinfo *sbinfo)
{
	aufs_bindex_t bmax;
	struct au_branch **br;

	AuRwMustWriteLock(&sbinfo->si_rwsem);

	bmax = sbinfo->si_bend + 1;
	br = sbinfo->si_branch;
	while (bmax--)
		au_br_do_free(*br++);
}

/*
 * find the index of a branch which is specified by @br_id.
 */
int au_br_index(struct super_block *sb, aufs_bindex_t br_id)
{
	aufs_bindex_t bindex, bend;

	bend = au_sbend(sb);
	for (bindex = 0; bindex <= bend; bindex++)
		if (au_sbr_id(sb, bindex) == br_id)
			return bindex;
	return -1;
}

/* ---------------------------------------------------------------------- */

/*
 * add a branch
 */

static int test_overlap(struct super_block *sb, struct dentry *h_adding,
			struct dentry *h_root)
{
	if (unlikely(h_adding == h_root))
		return 1;
	if (h_adding->d_sb != h_root->d_sb)
		return 0;
	return au_test_subdir(h_adding, h_root)
		|| au_test_subdir(h_root, h_adding);
}

/*
 * returns a newly allocated branch. @new_nbranch is a number of branches
 * after adding a branch.
 */
static struct au_branch *au_br_alloc(struct super_block *sb, int new_nbranch,
				     int perm)
{
	struct au_branch *add_branch;
	struct dentry *root;
	int err;

	err = -ENOMEM;
	root = sb->s_root;
	add_branch = kmalloc(sizeof(*add_branch), GFP_NOFS);
	if (unlikely(!add_branch))
		goto out;

	err = au_sbr_realloc(au_sbi(sb), new_nbranch);
	if (!err)
		err = au_di_realloc(au_di(root), new_nbranch);
	if (!err)
		err = au_ii_realloc(au_ii(root->d_inode), new_nbranch);
	if (!err)
		return add_branch; /* success */

	kfree(add_branch);
out:
	return ERR_PTR(err);
}

/*
 * returns:
 * 0: success, the caller will add it
 * plus: success, it is already unified, the caller should ignore it
 * minus: error
 */
static int test_add(struct super_block *sb, struct au_opt_add *add)
{
	int err;
	aufs_bindex_t bend, bindex;
	struct dentry *root;
	struct inode *inode;

	root = sb->s_root;
	bend = au_sbend(sb);
	if (unlikely(bend >= 0
		     && au_find_dbindex(root, add->path.dentry) >= 0)) {
		err = -EINVAL;
		pr_err("%s duplicated\n", add->pathname);
		goto out;
	}

	err = -ENOSPC; /* -E2BIG; */
	if (unlikely(AUFS_BRANCH_MAX <= add->bindex
		     || AUFS_BRANCH_MAX - 1 <= bend)) {
		pr_err("number of branches exceeded %s\n", add->pathname);
		goto out;
	}

	err = -EDOM;
	if (unlikely(add->bindex < 0 || bend + 1 < add->bindex)) {
		pr_err("bad index %d\n", add->bindex);
		goto out;
	}

	inode = add->path.dentry->d_inode;
	err = -ENOENT;
	if (unlikely(!inode->i_nlink)) {
		pr_err("no existence %s\n", add->pathname);
		goto out;
	}

	err = -EINVAL;
	if (unlikely(inode->i_sb == sb)) {
		pr_err("%s must be outside\n", add->pathname);
		goto out;
	}

	if (unlikely(au_test_fs_unsuppoted(inode->i_sb))) {
		pr_err("unsupported filesystem, %s (%s)\n",
		       add->pathname, au_sbtype(inode->i_sb));
		goto out;
	}

	if (unlikely(inode->i_sb->s_stack_depth)) {
		pr_err("already stacked, %s (%s)\n",
		       add->pathname, au_sbtype(inode->i_sb));
		goto out;
	}

	if (bend < 0)
		return 0; /* success */

	err = -EINVAL;
	for (bindex = 0; bindex <= bend; bindex++)
		if (unlikely(test_overlap(sb, add->path.dentry,
					  au_h_dptr(root, bindex)))) {
			pr_err("%s is overlapped\n", add->pathname);
			goto out;
		}

	err = 0;

out:
	return err;
}

/* initialize a new branch */
static int au_br_init(struct au_branch *br, struct super_block *sb,
		      struct au_opt_add *add)
{
	int err;

	err = 0;
	memset(&br->br_xino, 0, sizeof(br->br_xino));
	mutex_init(&br->br_xino.xi_nondir_mtx);
	br->br_perm = add->perm;
	br->br_path = add->path; /* set first, path_get() later */
	atomic_set(&br->br_count, 0);
	br->br_id = au_new_br_id(sb);
	AuDebugOn(br->br_id < 0);

	if (au_opt_test(au_mntflags(sb), XINO)) {
		err = au_xino_br(sb, br, add->path.dentry->d_inode->i_ino,
				 au_sbr(sb, 0)->br_xino.xi_file, /*do_test*/1);
		if (unlikely(err)) {
			AuDebugOn(br->br_xino.xi_file);
			goto out_err;
		}
	}

	sysaufs_br_init(br);
	path_get(&br->br_path);
	goto out; /* success */

out_err:
	memset(&br->br_path, 0, sizeof(br->br_path));
out:
	return err;
}

static void au_br_do_add_brp(struct au_sbinfo *sbinfo, aufs_bindex_t bindex,
			     struct au_branch *br, aufs_bindex_t bend,
			     aufs_bindex_t amount)
{
	struct au_branch **brp;

	AuRwMustWriteLock(&sbinfo->si_rwsem);

	brp = sbinfo->si_branch + bindex;
	memmove(brp + 1, brp, sizeof(*brp) * amount);
	*brp = br;
	sbinfo->si_bend++;
	if (unlikely(bend < 0))
		sbinfo->si_bend = 0;
}

static void au_br_do_add_hdp(struct au_dinfo *dinfo, aufs_bindex_t bindex,
			     aufs_bindex_t bend, aufs_bindex_t amount)
{
	struct au_hdentry *hdp;

	AuRwMustWriteLock(&dinfo->di_rwsem);

	hdp = dinfo->di_hdentry + bindex;
	memmove(hdp + 1, hdp, sizeof(*hdp) * amount);
	au_h_dentry_init(hdp);
	dinfo->di_bend++;
	if (unlikely(bend < 0))
		dinfo->di_bstart = 0;
}

static void au_br_do_add_hip(struct au_iinfo *iinfo, aufs_bindex_t bindex,
			     aufs_bindex_t bend, aufs_bindex_t amount)
{
	struct au_hinode *hip;

	AuRwMustWriteLock(&iinfo->ii_rwsem);

	hip = iinfo->ii_hinode + bindex;
	memmove(hip + 1, hip, sizeof(*hip) * amount);
	hip->hi_inode = NULL;
	iinfo->ii_bend++;
	if (unlikely(bend < 0))
		iinfo->ii_bstart = 0;
}

static void au_br_do_add(struct super_block *sb, struct au_branch *br,
			 aufs_bindex_t bindex)
{
	struct dentry *root, *h_dentry;
	struct inode *root_inode;
	aufs_bindex_t bend, amount;

	root = sb->s_root;
	root_inode = root->d_inode;
	bend = au_sbend(sb);
	amount = bend + 1 - bindex;
	h_dentry = au_br_dentry(br);
	au_br_do_add_brp(au_sbi(sb), bindex, br, bend, amount);
	au_br_do_add_hdp(au_di(root), bindex, bend, amount);
	au_br_do_add_hip(au_ii(root_inode), bindex, bend, amount);
	au_set_h_dptr(root, bindex, dget(h_dentry));
	au_set_h_iptr(root_inode, bindex, au_igrab(h_dentry->d_inode),
		      /*flags*/0);
}

int au_br_add(struct super_block *sb, struct au_opt_add *add)
{
	int err;
	aufs_bindex_t bend, add_bindex;
	struct dentry *root, *h_dentry;
	struct inode *root_inode;
	struct au_branch *add_branch;

	root = sb->s_root;
	root_inode = root->d_inode;
	IMustLock(root_inode);
	err = test_add(sb, add);
	if (unlikely(err < 0))
		goto out;
	if (err) {
		err = 0;
		goto out; /* success */
	}

	bend = au_sbend(sb);
	add_branch = au_br_alloc(sb, bend + 2, add->perm);
	err = PTR_ERR(add_branch);
	if (IS_ERR(add_branch))
		goto out;

	err = au_br_init(add_branch, sb, add);
	if (unlikely(err)) {
		au_br_do_free(add_branch);
		goto out;
	}

	add_bindex = add->bindex;
	au_br_do_add(sb, add_branch, add_bindex);

	h_dentry = add->path.dentry;
	if (!add_bindex)
		sb->s_maxbytes = h_dentry->d_sb->s_maxbytes;

	/*
	 * this test/set prevents aufs from handling unnecesary notify events
	 * of xino files, in case of re-adding a writable branch which was
	 * once detached from aufs.
	 */
	if (au_xino_brid(sb) < 0
	    /* && au_br_writable(add_branch->br_perm) re-commit later */
	    && !au_test_fs_bad_xino(h_dentry->d_sb)
	    && add_branch->br_xino.xi_file
	    && add_branch->br_xino.xi_file->f_path.dentry->d_parent == h_dentry)
		au_xino_brid_set(sb, add_branch->br_id);

out:
	return err;
}
