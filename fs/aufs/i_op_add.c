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
 * inode operations (add entry)
 */

#include "aufs.h"

/*
 * final procedure of adding a new entry, except link(2).
 * remove whiteout, instantiate, copyup the parent dir's times and size
 * and update version.
 * if it failed, re-create the removed whiteout.
 */
static int epilog(struct inode *dir, aufs_bindex_t bindex,
		  struct dentry *wh_dentry, struct dentry *dentry)
{
	int err, rerr;
	aufs_bindex_t bwh;
	struct path h_path;
	struct super_block *sb;
	struct inode *inode, *h_dir;
	struct dentry *wh;

	bwh = -1;
	sb = dir->i_sb;
	if (wh_dentry) {
		h_dir = wh_dentry->d_parent->d_inode; /* dir inode is locked */
		IMustLock(h_dir);
		AuDebugOn(au_h_iptr(dir, bindex) != h_dir);
		bwh = au_dbwh(dentry);
		h_path.dentry = wh_dentry;
		h_path.mnt = au_sbr_mnt(sb, bindex);
		err = au_wh_unlink_dentry(au_h_iptr(dir, bindex), &h_path,
					  dentry);
		if (unlikely(err))
			goto out;
	}

	inode = au_new_inode(dentry, /*must_new*/1);
	if (!IS_ERR(inode)) {
		d_instantiate(dentry, inode);
		dir = dentry->d_parent->d_inode; /* dir inode is locked */
		IMustLock(dir);
		if (au_ibstart(dir) == au_dbstart(dentry))
			au_cpup_attr_timesizes(dir);
		dir->i_version++;
		return 0; /* success */
	}

	err = PTR_ERR(inode);
	if (!wh_dentry)
		goto out;

	/* revert */
	/* dir inode is locked */
	wh = au_wh_create(dentry, bwh, wh_dentry->d_parent);
	rerr = PTR_ERR(wh);
	if (IS_ERR(wh)) {
		AuIOErr("%pd reverting whiteout failed(%d, %d)\n",
			dentry, err, rerr);
		err = -EIO;
	} else
		dput(wh);

out:
	return err;
}

static int au_d_may_add(struct dentry *dentry)
{
	int err;

	err = 0;
	if (unlikely(d_unhashed(dentry)))
		err = -ENOENT;
	if (unlikely(dentry->d_inode))
		err = -EEXIST;
	return err;
}

/*
 * simple tests for the adding inode operations.
 * following the checks in vfs, plus the parent-child relationship.
 */
int au_may_add(struct dentry *dentry, aufs_bindex_t bindex,
	       struct dentry *h_parent, int isdir)
{
	int err;
	umode_t h_mode;
	struct dentry *h_dentry;
	struct inode *h_inode;

	err = -ENAMETOOLONG;
	if (unlikely(dentry->d_name.len > AUFS_MAX_NAMELEN))
		goto out;

	h_dentry = au_h_dptr(dentry, bindex);
	h_inode = h_dentry->d_inode;
	if (!dentry->d_inode) {
		err = -EEXIST;
		if (unlikely(h_inode))
			goto out;
	} else {
		/* rename(2) case */
		err = -EIO;
		if (unlikely(!h_inode || !h_inode->i_nlink))
			goto out;

		h_mode = h_inode->i_mode;
		if (!isdir) {
			err = -EISDIR;
			if (unlikely(S_ISDIR(h_mode)))
				goto out;
		} else if (unlikely(!S_ISDIR(h_mode))) {
			err = -ENOTDIR;
			goto out;
		}
	}

	err = 0;
	/* expected parent dir is locked */
	if (unlikely(h_parent != h_dentry->d_parent))
		err = -EIO;

out:
	AuTraceErr(err);
	return err;
}

/*
 * initial procedure of adding a new entry.
 * prepare writable branch and the parent dir, lock it,
 * and lookup whiteout for the new entry.
 */
static struct dentry*
lock_hdir_lkup_wh(struct dentry *dentry, struct au_dtime *dt,
		  struct dentry *src_dentry, struct au_pin *pin,
		  struct au_wr_dir_args *wr_dir_args)
{
	struct dentry *wh_dentry, *h_parent;
	struct super_block *sb;
	struct au_branch *br;
	int err;
	unsigned int udba;
	aufs_bindex_t bcpup;

	AuDbg("%pd\n", dentry);

	err = au_wr_dir(dentry, src_dentry, wr_dir_args);
	bcpup = err;
	wh_dentry = ERR_PTR(err);
	if (unlikely(err < 0))
		goto out;

	sb = dentry->d_sb;
	udba = au_opt_udba(sb);
	err = au_pin(pin, dentry, bcpup, udba,
		     AuPin_DI_LOCKED | AuPin_MNT_WRITE);
	wh_dentry = ERR_PTR(err);
	if (unlikely(err))
		goto out;

	h_parent = au_pinned_h_parent(pin);
	if (udba != AuOpt_UDBA_NONE
	    && au_dbstart(dentry) == bcpup)
		err = au_may_add(dentry, bcpup, h_parent,
				 au_ftest_wrdir(wr_dir_args->flags, ISDIR));
	else if (unlikely(dentry->d_name.len > AUFS_MAX_NAMELEN))
		err = -ENAMETOOLONG;
	wh_dentry = ERR_PTR(err);
	if (unlikely(err))
		goto out_unpin;

	br = au_sbr(sb, bcpup);
	if (dt) {
		struct path tmp = {
			.dentry	= h_parent,
			.mnt	= au_br_mnt(br)
		};
		au_dtime_store(dt, au_pinned_parent(pin), &tmp);
	}

	wh_dentry = NULL;
	if (bcpup != au_dbwh(dentry))
		goto out; /* success */

	/*
	 * ENAMETOOLONG here means that if we allowed create such name, then it
	 * would not be able to removed in the future. So we don't allow such
	 * name here and we don't handle ENAMETOOLONG differently here.
	 */
	wh_dentry = au_wh_lkup(h_parent, &dentry->d_name, br);

out_unpin:
	if (IS_ERR(wh_dentry))
		au_unpin(pin);
out:
	return wh_dentry;
}

/* ---------------------------------------------------------------------- */

enum { Mknod, Symlink, Creat };
struct simple_arg {
	int type;
	union {
		struct {
			umode_t mode;
			bool want_excl;
		} c;
		struct {
			const char *symname;
		} s;
		struct {
			umode_t mode;
			dev_t dev;
		} m;
	} u;
};

static int add_simple(struct inode *dir, struct dentry *dentry,
		      struct simple_arg *arg)
{
	int err, rerr;
	aufs_bindex_t bstart;
	unsigned char created;
	struct dentry *wh_dentry, *parent;
	struct inode *h_dir;
	/* to reuduce stack size */
	struct {
		struct au_dtime dt;
		struct au_pin pin;
		struct path h_path;
		struct au_wr_dir_args wr_dir_args;
	} *a;

	AuDbg("%pd\n", dentry);
	IMustLock(dir);

	err = -ENOMEM;
	a = kmalloc(sizeof(*a), GFP_NOFS);
	if (unlikely(!a))
		goto out;
	a->wr_dir_args.force_btgt = -1;
	a->wr_dir_args.flags = AuWrDir_ADD_ENTRY;

	parent = dentry->d_parent; /* dir inode is locked */
	err = aufs_read_lock(dentry, AuLock_DW | AuLock_GEN);
	if (unlikely(err))
		goto out_free;
	err = au_d_may_add(dentry);
	if (unlikely(err))
		goto out_unlock;
	di_write_lock_parent(parent);
	wh_dentry = lock_hdir_lkup_wh(dentry, &a->dt, /*src_dentry*/NULL,
				      &a->pin, &a->wr_dir_args);
	err = PTR_ERR(wh_dentry);
	if (IS_ERR(wh_dentry))
		goto out_parent;

	bstart = au_dbstart(dentry);
	a->h_path.dentry = au_h_dptr(dentry, bstart);
	a->h_path.mnt = au_sbr_mnt(dentry->d_sb, bstart);
	h_dir = au_pinned_h_dir(&a->pin);
	switch (arg->type) {
	case Creat:
		err = vfsub_create(h_dir, &a->h_path, arg->u.c.mode,
				   arg->u.c.want_excl);
		break;
	case Symlink:
		err = vfsub_symlink(h_dir, &a->h_path, arg->u.s.symname);
		break;
	case Mknod:
		err = vfsub_mknod(h_dir, &a->h_path, arg->u.m.mode,
				  arg->u.m.dev);
		break;
	default:
		BUG();
	}
	created = !err;
	if (!err)
		err = epilog(dir, bstart, wh_dentry, dentry);

	/* revert */
	if (unlikely(created && err && a->h_path.dentry->d_inode)) {
		/* no delegation since it is just created */
		rerr = vfsub_unlink(h_dir, &a->h_path, /*delegated*/NULL,
				    /*force*/0);
		if (rerr) {
			AuIOErr("%pd revert failure(%d, %d)\n",
				dentry, err, rerr);
			err = -EIO;
		}
		au_dtime_revert(&a->dt);
	}

	au_unpin(&a->pin);
	dput(wh_dentry);

out_parent:
	di_write_unlock(parent);
out_unlock:
	if (unlikely(err)) {
		au_update_dbstart(dentry);
		d_drop(dentry);
	}
	aufs_read_unlock(dentry, AuLock_DW);
out_free:
	kfree(a);
out:
	return err;
}

int aufs_mknod(struct inode *dir, struct dentry *dentry, umode_t mode,
	       dev_t dev)
{
	struct simple_arg arg = {
		.type = Mknod,
		.u.m = {
			.mode	= mode,
			.dev	= dev
		}
	};
	return add_simple(dir, dentry, &arg);
}

int aufs_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	struct simple_arg arg = {
		.type = Symlink,
		.u.s.symname = symname
	};
	return add_simple(dir, dentry, &arg);
}

int aufs_create(struct inode *dir, struct dentry *dentry, umode_t mode,
		bool want_excl)
{
	struct simple_arg arg = {
		.type = Creat,
		.u.c = {
			.mode		= mode,
			.want_excl	= want_excl
		}
	};
	return add_simple(dir, dentry, &arg);
}

int aufs_tmpfile(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	int err;
	aufs_bindex_t bindex;
	struct super_block *sb;
	struct dentry *parent, *h_parent, *h_dentry;
	struct inode *h_dir, *inode;
	struct vfsmount *h_mnt;
	struct au_wr_dir_args wr_dir_args = {
		.force_btgt	= -1,
		.flags		= AuWrDir_TMPFILE
	};

	/* copy-up may happen */
	mutex_lock(&dir->i_mutex);

	sb = dir->i_sb;
	err = si_read_lock(sb, AuLock_FLUSH | AuLock_NOPLM);
	if (unlikely(err))
		goto out;

	err = au_di_init(dentry);
	if (unlikely(err))
		goto out_si;

	err = -EBUSY;
	parent = d_find_any_alias(dir);
	AuDebugOn(!parent);
	di_write_lock_parent(parent);
	if (unlikely(parent->d_inode != dir))
		goto out_parent;

	err = au_digen_test(parent, au_sigen(sb));
	if (unlikely(err))
		goto out_parent;

	bindex = au_dbstart(parent);
	au_set_dbstart(dentry, bindex);
	au_set_dbend(dentry, bindex);
	err = au_wr_dir(dentry, /*src_dentry*/NULL, &wr_dir_args);
	bindex = err;
	if (unlikely(err < 0))
		goto out_parent;

	err = -EOPNOTSUPP;
	h_dir = au_h_iptr(dir, bindex);
	if (unlikely(!h_dir->i_op->tmpfile))
		goto out_parent;

	h_mnt = au_sbr_mnt(sb, bindex);
	err = vfsub_mnt_want_write(h_mnt);
	if (unlikely(err))
		goto out_parent;

	h_parent = au_h_dptr(parent, bindex);
	err = inode_permission(h_parent->d_inode, MAY_WRITE | MAY_EXEC);
	if (unlikely(err))
		goto out_mnt;

	err = -ENOMEM;
	h_dentry = d_alloc(h_parent, &dentry->d_name);
	if (unlikely(!h_dentry))
		goto out_mnt;

	err = h_dir->i_op->tmpfile(h_dir, h_dentry, mode);
	if (unlikely(err))
		goto out_dentry;

	au_set_dbstart(dentry, bindex);
	au_set_dbend(dentry, bindex);
	au_set_h_dptr(dentry, bindex, dget(h_dentry));
	inode = au_new_inode(dentry, /*must_new*/1);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		au_set_h_dptr(dentry, bindex, NULL);
		au_set_dbstart(dentry, -1);
		au_set_dbend(dentry, -1);
	} else {
		if (!inode->i_nlink)
			set_nlink(inode, 1);
		d_tmpfile(dentry, inode);
		au_di(dentry)->di_tmpfile = 1;

		/* update without i_mutex */
		if (au_ibstart(dir) == au_dbstart(dentry))
			au_cpup_attr_timesizes(dir);
	}

out_dentry:
	dput(h_dentry);
out_mnt:
	vfsub_mnt_drop_write(h_mnt);
out_parent:
	di_write_unlock(parent);
	dput(parent);
	di_write_unlock(dentry);
	if (!err)
#if 0
		/* verbose coding for lock class name */
		au_rw_class(&au_di(dentry)->di_rwsem,
			    au_lc_key + AuLcNonDir_DIINFO);
#else
		;
#endif
	else {
		au_di_fin(dentry);
		dentry->d_fsdata = NULL;
	}
out_si:
	si_read_unlock(sb);
out:
	mutex_unlock(&dir->i_mutex);
	return err;
}

/* ---------------------------------------------------------------------- */

int aufs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	int err, rerr;
	aufs_bindex_t bindex;
	unsigned char diropq;
	struct path h_path;
	struct dentry *wh_dentry, *parent, *opq_dentry;
	struct mutex *h_mtx;
	struct super_block *sb;
	struct {
		struct au_pin pin;
		struct au_dtime dt;
	} *a; /* reduce the stack usage */
	struct au_wr_dir_args wr_dir_args = {
		.force_btgt	= -1,
		.flags		= AuWrDir_ADD_ENTRY | AuWrDir_ISDIR
	};

	IMustLock(dir);

	err = -ENOMEM;
	a = kmalloc(sizeof(*a), GFP_NOFS);
	if (unlikely(!a))
		goto out;

	err = aufs_read_lock(dentry, AuLock_DW | AuLock_GEN);
	if (unlikely(err))
		goto out_free;
	err = au_d_may_add(dentry);
	if (unlikely(err))
		goto out_unlock;

	parent = dentry->d_parent; /* dir inode is locked */
	di_write_lock_parent(parent);
	wh_dentry = lock_hdir_lkup_wh(dentry, &a->dt, /*src_dentry*/NULL,
				      &a->pin, &wr_dir_args);
	err = PTR_ERR(wh_dentry);
	if (IS_ERR(wh_dentry))
		goto out_parent;

	sb = dentry->d_sb;
	bindex = au_dbstart(dentry);
	h_path.dentry = au_h_dptr(dentry, bindex);
	h_path.mnt = au_sbr_mnt(sb, bindex);
	err = vfsub_mkdir(au_pinned_h_dir(&a->pin), &h_path, mode);
	if (unlikely(err))
		goto out_unpin;

	/* make the dir opaque */
	diropq = 0;
	h_mtx = &h_path.dentry->d_inode->i_mutex;
	if (wh_dentry) {
		mutex_lock_nested(h_mtx, AuLsc_I_CHILD);
		opq_dentry = au_diropq_create(dentry, bindex);
		mutex_unlock(h_mtx);
		err = PTR_ERR(opq_dentry);
		if (IS_ERR(opq_dentry))
			goto out_dir;
		dput(opq_dentry);
		diropq = 1;
	}

	err = epilog(dir, bindex, wh_dentry, dentry);
	if (!err) {
		inc_nlink(dir);
		goto out_unpin; /* success */
	}

	/* revert */
	if (diropq) {
		AuLabel(revert opq);
		mutex_lock_nested(h_mtx, AuLsc_I_CHILD);
		rerr = au_diropq_remove(dentry, bindex);
		mutex_unlock(h_mtx);
		if (rerr) {
			AuIOErr("%pd reverting diropq failed(%d, %d)\n",
				dentry, err, rerr);
			err = -EIO;
		}
	}

out_dir:
	AuLabel(revert dir);
	rerr = vfsub_rmdir(au_pinned_h_dir(&a->pin), &h_path);
	if (rerr) {
		AuIOErr("%pd reverting dir failed(%d, %d)\n",
			dentry, err, rerr);
		err = -EIO;
	}
	au_dtime_revert(&a->dt);
out_unpin:
	au_unpin(&a->pin);
	dput(wh_dentry);
out_parent:
	di_write_unlock(parent);
out_unlock:
	if (unlikely(err)) {
		au_update_dbstart(dentry);
		d_drop(dentry);
	}
	aufs_read_unlock(dentry, AuLock_DW);
out_free:
	kfree(a);
out:
	return err;
}
