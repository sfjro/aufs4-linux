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
 * whiteout for logical deletion and opaque directory
 */

#include "aufs.h"

#define WH_MASK			S_IRUGO

/*
 * If a directory contains this file, then it is opaque.  We start with the
 * .wh. flag so that it is blocked by lookup.
 */
static struct qstr diropq_name = QSTR_INIT(AUFS_WH_DIROPQ,
					   sizeof(AUFS_WH_DIROPQ) - 1);

/*
 * generate whiteout name, which is NOT terminated by NULL.
 * @name: original d_name.name
 * @len: original d_name.len
 * @wh: whiteout qstr
 * returns zero when succeeds, otherwise error.
 * succeeded value as wh->name should be freed by kfree().
 */
int au_wh_name_alloc(struct qstr *wh, const struct qstr *name)
{
	char *p;

	if (unlikely(name->len > PATH_MAX - AUFS_WH_PFX_LEN))
		return -ENAMETOOLONG;

	wh->len = name->len + AUFS_WH_PFX_LEN;
	p = kmalloc(wh->len, GFP_NOFS);
	wh->name = p;
	if (p) {
		memcpy(p, AUFS_WH_PFX, AUFS_WH_PFX_LEN);
		memcpy(p + AUFS_WH_PFX_LEN, name->name, name->len);
		/* smp_mb(); */
		return 0;
	}
	return -ENOMEM;
}

/* ---------------------------------------------------------------------- */

/*
 * test if the @wh_name exists under @h_parent.
 * @try_sio specifies the necessary of super-io.
 */
int au_wh_test(struct dentry *h_parent, struct qstr *wh_name, int try_sio)
{
	int err;
	struct dentry *wh_dentry;

#if 0 /* re-commit later */
	if (!try_sio)
		wh_dentry = vfsub_lkup_one(wh_name, h_parent);
	else
		wh_dentry = au_sio_lkup_one(wh_name, h_parent);
#else
	wh_dentry = ERR_PTR(-ENOENT);
#endif
	err = PTR_ERR(wh_dentry);
	if (IS_ERR(wh_dentry)) {
		if (err == -ENAMETOOLONG)
			err = 0;
		goto out;
	}

	err = 0;
	if (!wh_dentry->d_inode)
		goto out_wh; /* success */

	err = 1;
	if (d_is_reg(wh_dentry))
		goto out_wh; /* success */

	err = -EIO;
	AuIOErr("%pd Invalid whiteout entry type 0%o.\n",
		wh_dentry, wh_dentry->d_inode->i_mode);

out_wh:
	dput(wh_dentry);
out:
	return err;
}

/* ---------------------------------------------------------------------- */
/*
 * functions for removing a whiteout
 */

static int do_unlink_wh(struct inode *h_dir, struct path *h_path)
{
	int err, force;
	struct inode *delegated;

	/*
	 * forces superio when the dir has a sticky bit.
	 * this may be a violation of unix fs semantics.
	 */
	force = (h_dir->i_mode & S_ISVTX)
		&& !uid_eq(current_fsuid(), h_path->dentry->d_inode->i_uid);
	delegated = NULL;
	err = vfsub_unlink(h_dir, h_path, &delegated, force);
	if (unlikely(err == -EWOULDBLOCK)) {
		pr_warn("cannot retry for NFSv4 delegation"
			" for an internal unlink\n");
		iput(delegated);
	}
	return err;
}

int au_wh_unlink_dentry(struct inode *h_dir, struct path *h_path,
			struct dentry *dentry)
{
	int err;

	err = do_unlink_wh(h_dir, h_path);
	if (!err && dentry)
		au_set_dbwh(dentry, -1);

	return err;
}

/* ---------------------------------------------------------------------- */
/*
 * whiteouts are all hard-linked usually.
 * when its link count reaches a ceiling, we create a new whiteout base
 * asynchronously.
 */

struct reinit_br_wh {
	struct super_block *sb;
	struct au_branch *br;
};

static void reinit_br_wh(void *arg)
{
	int err;
	aufs_bindex_t bindex;
	struct path h_path;
	struct reinit_br_wh *a = arg;
	struct au_wbr *wbr;
	struct inode *dir, *delegated;
	struct dentry *h_root;
	struct au_hinode *hdir;

	err = 0;
	wbr = a->br->br_wbr;
	/* big aufs lock */
	si_noflush_write_lock(a->sb);
	if (!au_br_writable(a->br->br_perm))
		goto out;
	bindex = au_br_index(a->sb, a->br->br_id);
	if (unlikely(bindex < 0))
		goto out;

	di_read_lock_parent(a->sb->s_root, AuLock_IR);
	dir = a->sb->s_root->d_inode;
	hdir = au_hi(dir, bindex);
	h_root = au_h_dptr(a->sb->s_root, bindex);
	AuDebugOn(h_root != au_br_dentry(a->br));

	mutex_lock_nested(&hdir->hi_inode->i_mutex, AuLsc_I_PARENT);
	wbr_wh_write_lock(wbr);
#if 0 /* re-commit later */
	err = au_h_verify(wbr->wbr_whbase, /*udba dummy*/0, hdir->hi_inode,
			  h_root, a->br);
#else
	err = 0;
#endif
	if (!err) {
		h_path.dentry = wbr->wbr_whbase;
		h_path.mnt = au_br_mnt(a->br);
		delegated = NULL;
		err = vfsub_unlink(hdir->hi_inode, &h_path, &delegated,
				   /*force*/0);
		if (unlikely(err == -EWOULDBLOCK)) {
			pr_warn("cannot retry for NFSv4 delegation"
				" for an internal unlink\n");
			iput(delegated);
		}
	} else {
		pr_warn("%pd is moved, ignored\n", wbr->wbr_whbase);
		err = 0;
	}
	dput(wbr->wbr_whbase);
	wbr->wbr_whbase = NULL;
#if 0 /* re-commit later */
	if (!err)
		err = au_wh_init(a->br, a->sb);
#endif
	wbr_wh_write_unlock(wbr);
	mutex_unlock(&hdir->hi_inode->i_mutex);
	di_read_unlock(a->sb->s_root, AuLock_IR);

out:
	if (wbr)
		atomic_dec(&wbr->wbr_wh_running);
	atomic_dec(&a->br->br_count);
	si_write_unlock(a->sb);
	au_nwt_done(&au_sbi(a->sb)->si_nowait);
	kfree(arg);
	if (unlikely(err))
		AuIOErr("err %d\n", err);
}

static void kick_reinit_br_wh(struct super_block *sb, struct au_branch *br)
{
	int do_dec, wkq_err;
	struct reinit_br_wh *arg;

	do_dec = 1;
	if (atomic_inc_return(&br->br_wbr->wbr_wh_running) != 1)
		goto out;

	/* ignore ENOMEM */
	arg = kmalloc(sizeof(*arg), GFP_NOFS);
	if (arg) {
		/*
		 * dec(wh_running), kfree(arg) and dec(br_count)
		 * in reinit function
		 */
		arg->sb = sb;
		arg->br = br;
		atomic_inc(&br->br_count);
		wkq_err = au_wkq_nowait(reinit_br_wh, arg, sb, /*flags*/0);
		if (unlikely(wkq_err)) {
			atomic_dec(&br->br_wbr->wbr_wh_running);
			atomic_dec(&br->br_count);
			kfree(arg);
		}
		do_dec = 0;
	}

out:
	if (do_dec)
		atomic_dec(&br->br_wbr->wbr_wh_running);
}

/* ---------------------------------------------------------------------- */

/*
 * create the whiteout @wh.
 */
static int link_or_create_wh(struct super_block *sb, aufs_bindex_t bindex,
			     struct dentry *wh)
{
	int err;
	struct path h_path = {
		.dentry = wh
	};
	struct au_branch *br;
	struct au_wbr *wbr;
	struct dentry *h_parent;
	struct inode *h_dir, *delegated;

	h_parent = wh->d_parent; /* dir inode is locked */
	h_dir = h_parent->d_inode;
	IMustLock(h_dir);

	br = au_sbr(sb, bindex);
	h_path.mnt = au_br_mnt(br);
	wbr = br->br_wbr;
	wbr_wh_read_lock(wbr);
	if (wbr->wbr_whbase) {
		delegated = NULL;
		err = vfsub_link(wbr->wbr_whbase, h_dir, &h_path, &delegated);
		if (unlikely(err == -EWOULDBLOCK)) {
			pr_warn("cannot retry for NFSv4 delegation"
				" for an internal link\n");
			iput(delegated);
		}
		if (!err || err != -EMLINK)
			goto out;

		/* link count full. re-initialize br_whbase. */
		kick_reinit_br_wh(sb, br);
	}

	/* return this error in this context */
	err = vfsub_create(h_dir, &h_path, WH_MASK, /*want_excl*/true);

out:
	wbr_wh_read_unlock(wbr);
	return err;
}

/* ---------------------------------------------------------------------- */

/*
 * lookup whiteout dentry.
 * @h_parent: lower parent dentry which must exist and be locked
 * @base_name: name of dentry which will be whiteouted
 * returns dentry for whiteout.
 */
struct dentry *au_wh_lkup(struct dentry *h_parent, struct qstr *base_name,
			  struct au_branch *br)
{
	int err;
	struct qstr wh_name;
	struct dentry *wh_dentry;

	err = au_wh_name_alloc(&wh_name, base_name);
	wh_dentry = ERR_PTR(err);
	if (!err) {
		wh_dentry = vfsub_lkup_one(&wh_name, h_parent);
		kfree(wh_name.name);
	}
	return wh_dentry;
}

/*
 * link/create a whiteout for @dentry on @bindex.
 */
struct dentry *au_wh_create(struct dentry *dentry, aufs_bindex_t bindex,
			    struct dentry *h_parent)
{
	struct dentry *wh_dentry;
	struct super_block *sb;
	int err;

	sb = dentry->d_sb;
	wh_dentry = au_wh_lkup(h_parent, &dentry->d_name, au_sbr(sb, bindex));
	if (!IS_ERR(wh_dentry) && !wh_dentry->d_inode) {
		err = link_or_create_wh(sb, bindex, wh_dentry);
		if (!err)
			au_set_dbwh(dentry, bindex);
		else {
			dput(wh_dentry);
			wh_dentry = ERR_PTR(err);
		}
	}

	return wh_dentry;
}
