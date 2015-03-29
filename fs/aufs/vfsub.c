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
 * sub-routines for VFS
 */

#include <linux/namei.h>
#include <linux/security.h>
#include "aufs.h"

struct file *vfsub_dentry_open(struct path *path, int flags)
{
	struct file *file;

	file = dentry_open(path, flags /* | __FMODE_NONOTIFY */,
			   current_cred());
	if (!IS_ERR_OR_NULL(file)
	    && (file->f_mode & (FMODE_READ | FMODE_WRITE)) == FMODE_READ)
		i_readcount_inc(path->dentry->d_inode);

	return file;
}

struct file *vfsub_filp_open(const char *path, int oflags, int mode)
{
	struct file *file;

	lockdep_off();
	file = filp_open(path,
			 oflags /* | __FMODE_NONOTIFY */,
			 mode);
	lockdep_on();

	return file;
}

int vfsub_kern_path(const char *name, unsigned int flags, struct path *path)
{
	int err;

	err = kern_path(name, flags, path);
	return err;
}

struct dentry *vfsub_lookup_one_len(const char *name, struct dentry *parent,
				    int len)
{
	struct path path = {
		.mnt = NULL
	};

	/* VFS checks it too, but by WARN_ON_ONCE() */
	IMustLock(parent->d_inode);

	path.dentry = lookup_one_len(name, parent, len);
	if (IS_ERR(path.dentry))
		goto out;

out:
	AuTraceErrPtr(path.dentry);
	return path.dentry;
}

/* ---------------------------------------------------------------------- */

int vfsub_create(struct inode *dir, struct path *path, int mode, bool want_excl)
{
	int err;
	struct dentry *d;

	IMustLock(dir);

	d = path->dentry;
	path->dentry = d->d_parent;
	err = security_path_mknod(path, d, mode, 0);
	path->dentry = d;
	if (unlikely(err))
		goto out;

	lockdep_off();
	err = vfs_create(dir, path->dentry, mode, want_excl);
	lockdep_on();

out:
	return err;
}

static int au_test_nlink(struct inode *inode)
{
	const unsigned int link_max = UINT_MAX >> 1; /* rough margin */

	if (!au_test_fs_no_limit_nlink(inode->i_sb)
	    || inode->i_nlink < link_max)
		return 0;
	return -EMLINK;
}

int vfsub_link(struct dentry *src_dentry, struct inode *dir, struct path *path,
	       struct inode **delegated_inode)
{
	int err;
	struct dentry *d;

	IMustLock(dir);

	err = au_test_nlink(src_dentry->d_inode);
	if (unlikely(err))
		return err;

	/* we don't call may_linkat() */
	d = path->dentry;
	path->dentry = d->d_parent;
	err = security_path_link(src_dentry, path, d);
	path->dentry = d;
	if (unlikely(err))
		goto out;

	lockdep_off();
	err = vfs_link(src_dentry, dir, path->dentry, delegated_inode);
	lockdep_on();

out:
	return err;
}

int vfsub_mkdir(struct inode *dir, struct path *path, int mode)
{
	int err;
	struct dentry *d;

	IMustLock(dir);

	d = path->dentry;
	path->dentry = d->d_parent;
	err = security_path_mkdir(path, d, mode);
	path->dentry = d;
	if (unlikely(err))
		goto out;

	lockdep_off();
	err = vfs_mkdir(dir, path->dentry, mode);
	lockdep_on();

out:
	return err;
}

int vfsub_rmdir(struct inode *dir, struct path *path)
{
	int err;
	struct dentry *d;

	IMustLock(dir);

	d = path->dentry;
	path->dentry = d->d_parent;
	err = security_path_rmdir(path, d);
	path->dentry = d;
	if (unlikely(err))
		goto out;

	lockdep_off();
	err = vfs_rmdir(dir, path->dentry);
	lockdep_on();

out:
	return err;
}

/* ---------------------------------------------------------------------- */

int vfsub_iterate_dir(struct file *file, struct dir_context *ctx)
{
	int err;

	AuDbg("%pD, ctx{%pf, %llu}\n", file, ctx->actor, ctx->pos);

	lockdep_off();
	err = iterate_dir(file, ctx);
	lockdep_on();
	return err;
}

/* ---------------------------------------------------------------------- */

struct notify_change_args {
	int *errp;
	struct path *path;
	struct iattr *ia;
	struct inode **delegated_inode;
};

static void call_notify_change(void *args)
{
	struct notify_change_args *a = args;
	struct inode *h_inode;

	h_inode = a->path->dentry->d_inode;
	IMustLock(h_inode);

	*a->errp = -EPERM;
	if (!IS_IMMUTABLE(h_inode) && !IS_APPEND(h_inode)) {
		lockdep_off();
		*a->errp = notify_change(a->path->dentry, a->ia,
					 a->delegated_inode);
		lockdep_on();
	}
	AuTraceErr(*a->errp);
}

int vfsub_notify_change(struct path *path, struct iattr *ia,
			struct inode **delegated_inode)
{
	int err;
	struct notify_change_args args = {
		.errp			= &err,
		.path			= path,
		.ia			= ia,
		.delegated_inode	= delegated_inode
	};

	call_notify_change(&args);

	return err;
}

int vfsub_sio_notify_change(struct path *path, struct iattr *ia,
			    struct inode **delegated_inode)
{
	int err, wkq_err;
	struct notify_change_args args = {
		.errp			= &err,
		.path			= path,
		.ia			= ia,
		.delegated_inode	= delegated_inode
	};

	wkq_err = au_wkq_wait(call_notify_change, &args);
	if (unlikely(wkq_err))
		err = wkq_err;

	return err;
}

/* ---------------------------------------------------------------------- */

struct unlink_args {
	int *errp;
	struct inode *dir;
	struct path *path;
	struct inode **delegated_inode;
};

static void call_unlink(void *args)
{
	struct unlink_args *a = args;
	struct dentry *d = a->path->dentry;
	struct inode *h_inode;
	const int stop_sillyrename = (au_test_nfs(d->d_sb)
				      && au_dcount(d) == 1);

	IMustLock(a->dir);

	a->path->dentry = d->d_parent;
	*a->errp = security_path_unlink(a->path, d);
	a->path->dentry = d;
	if (unlikely(*a->errp))
		return;

	if (!stop_sillyrename)
		dget(d);
	h_inode = d->d_inode;
	if (h_inode)
		ihold(h_inode);

	lockdep_off();
	*a->errp = vfs_unlink(a->dir, d, a->delegated_inode);
	lockdep_on();

	if (!stop_sillyrename)
		dput(d);
	if (h_inode)
		iput(h_inode);

	AuTraceErr(*a->errp);
}

/*
 * @dir: must be locked.
 * @dentry: target dentry.
 */
int vfsub_unlink(struct inode *dir, struct path *path,
		 struct inode **delegated_inode, int force)
{
	int err;
	struct unlink_args args = {
		.errp			= &err,
		.dir			= dir,
		.path			= path,
		.delegated_inode	= delegated_inode
	};

	if (!force)
		call_unlink(&args);
	else {
		int wkq_err;

		wkq_err = au_wkq_wait(call_unlink, &args);
		if (unlikely(wkq_err))
			err = wkq_err;
	}

	return err;
}
