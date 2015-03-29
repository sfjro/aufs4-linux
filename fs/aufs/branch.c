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

#include <linux/statfs.h>
#include "aufs.h"

/*
 * free a single branch
 */
static void au_br_do_free(struct au_branch *br)
{
	int i;
	struct au_wbr *wbr;

	if (br->br_xino.xi_file)
		fput(br->br_xino.xi_file);
	mutex_destroy(&br->br_xino.xi_nondir_mtx);

	AuDebugOn(atomic_read(&br->br_count));

	wbr = br->br_wbr;
	if (wbr) {
		for (i = 0; i < AuBrWh_Last; i++)
			dput(wbr->wbr_wh[i]);
		AuDebugOn(atomic_read(&wbr->wbr_wh_running));
		AuRwDestroy(&wbr->wbr_wh_rwsem);
	}

	/* recursive lock, s_umount of branch's */
	lockdep_off();
	path_put(&br->br_path);
	lockdep_on();
	kfree(wbr);
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

	add_branch->br_wbr = NULL;
	if (au_br_writable(perm)) {
		/* may be freed separately at changing the branch permission */
		add_branch->br_wbr = kmalloc(sizeof(*add_branch->br_wbr),
					     GFP_NOFS);
		if (unlikely(!add_branch->br_wbr))
			goto out_br;
	}

	err = au_sbr_realloc(au_sbi(sb), new_nbranch);
	if (!err)
		err = au_di_realloc(au_di(root), new_nbranch);
	if (!err)
		err = au_ii_realloc(au_ii(root->d_inode), new_nbranch);
	if (!err)
		return add_branch; /* success */

	kfree(add_branch->br_wbr);
out_br:
	kfree(add_branch);
out:
	return ERR_PTR(err);
}

/*
 * test if the branch permission is legal or not.
 */
static int test_br(struct inode *inode, int brperm, char *path)
{
	int err;

	err = (au_br_writable(brperm) && IS_RDONLY(inode));
	if (!err)
		goto out;

	err = -EINVAL;
	pr_err("write permission for readonly mount or inode, %s\n", path);

out:
	return err;
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

	err = test_br(add->path.dentry->d_inode, add->perm, add->pathname);
	if (unlikely(err))
		goto out;

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

/*
 * initialize or clean the whiteouts for an adding branch
 */
static int au_br_init_wh(struct super_block *sb, struct au_branch *br,
			 int new_perm)
{
	int err, old_perm;
	aufs_bindex_t bindex;
	struct mutex *h_mtx;
	struct au_wbr *wbr;
	struct au_hinode *hdir;

	err = vfsub_mnt_want_write(au_br_mnt(br));
	if (unlikely(err))
		goto out;

	wbr = br->br_wbr;
	old_perm = br->br_perm;
	br->br_perm = new_perm;
	hdir = NULL;
	h_mtx = NULL;
	bindex = au_br_index(sb, br->br_id);
	if (0 <= bindex) {
		hdir = au_hi(sb->s_root->d_inode, bindex);
		mutex_lock_nested(&hdir->hi_inode->i_mutex, AuLsc_I_PARENT);
	} else {
		h_mtx = &au_br_dentry(br)->d_inode->i_mutex;
		mutex_lock_nested(h_mtx, AuLsc_I_PARENT);
	}
	if (!wbr)
		err = au_wh_init(br, sb);
	else {
		wbr_wh_write_lock(wbr);
		err = au_wh_init(br, sb);
		wbr_wh_write_unlock(wbr);
	}
	if (hdir)
		mutex_unlock(&hdir->hi_inode->i_mutex);
	else
		mutex_unlock(h_mtx);
	vfsub_mnt_drop_write(au_br_mnt(br));
	br->br_perm = old_perm;

	if (!err && wbr && !au_br_writable(new_perm)) {
		kfree(wbr);
		br->br_wbr = NULL;
	}

out:
	return err;
}

static int au_wbr_init(struct au_branch *br, struct super_block *sb,
		       int perm)
{
	int err;
	struct kstatfs kst;
	struct au_wbr *wbr;

	wbr = br->br_wbr;
	au_rw_init(&wbr->wbr_wh_rwsem);
	memset(wbr->wbr_wh, 0, sizeof(wbr->wbr_wh));
	atomic_set(&wbr->wbr_wh_running, 0);

	/*
	 * a limit for rmdir/rename a dir
	 * cf. AUFS_MAX_NAMELEN in include/uapi/linux/aufs_type.h
	 */
	err = vfs_statfs(&br->br_path, &kst);
	if (unlikely(err))
		goto out;
	err = -EINVAL;
	if (kst.f_namelen >= NAME_MAX)
		err = au_br_init_wh(sb, br, perm);
	else
		pr_err("%pd(%s), unsupported namelen %ld\n",
		       au_br_dentry(br),
		       au_sbtype(au_br_dentry(br)->d_sb), kst.f_namelen);

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

	if (au_br_writable(add->perm)) {
		err = au_wbr_init(br, sb, add->perm);
		if (unlikely(err))
			goto out_err;
	}

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
	if (!add_bindex) {
		au_cpup_attr_all(root_inode, /*force*/1);
		sb->s_maxbytes = h_dentry->d_sb->s_maxbytes;
	} else
		au_add_nlink(root_inode, h_dentry->d_inode);

	/*
	 * this test/set prevents aufs from handling unnecesary notify events
	 * of xino files, in case of re-adding a writable branch which was
	 * once detached from aufs.
	 */
	if (au_xino_brid(sb) < 0
	    && au_br_writable(add_branch->br_perm)
	    && !au_test_fs_bad_xino(h_dentry->d_sb)
	    && add_branch->br_xino.xi_file
	    && add_branch->br_xino.xi_file->f_path.dentry->d_parent == h_dentry)
		au_xino_brid_set(sb, add_branch->br_id);

out:
	return err;
}
