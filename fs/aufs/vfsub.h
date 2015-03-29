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

#ifndef __AUFS_VFSUB_H__
#define __AUFS_VFSUB_H__

#ifdef __KERNEL__

#include <linux/fs.h>
#include <linux/mount.h>
#include "debug.h"

/* ---------------------------------------------------------------------- */

/* lock subclass for lower inode */
/* default MAX_LOCKDEP_SUBCLASSES(8) is not enough */
/* reduce? gave up. */
enum {
	AuLsc_I_Begin = I_MUTEX_PARENT2, /* 5 */
	AuLsc_I_PARENT,		/* lower inode, parent first */
	AuLsc_I_PARENT2,	/* copyup dirs */
	AuLsc_I_PARENT3,	/* copyup wh */
	AuLsc_I_CHILD,
	AuLsc_I_CHILD2,
	AuLsc_I_End
};

/* to debug easier, do not make them inlined functions */
#define MtxMustLock(mtx)	AuDebugOn(!mutex_is_locked(mtx))
#define IMustLock(i)		MtxMustLock(&(i)->i_mutex)

/* ---------------------------------------------------------------------- */

struct file *vfsub_dentry_open(struct path *path, int flags);
struct file *vfsub_filp_open(const char *path, int oflags, int mode);
int vfsub_kern_path(const char *name, unsigned int flags, struct path *path);

struct dentry *vfsub_lookup_one_len(const char *name, struct dentry *parent,
				    int len);

struct vfsub_lkup_one_args {
	struct dentry **errp;
	struct qstr *name;
	struct dentry *parent;
};

static inline struct dentry *vfsub_lkup_one(struct qstr *name,
					    struct dentry *parent)
{
	return vfsub_lookup_one_len(name->name, parent, name->len);
}

void vfsub_call_lkup_one(void *args);

/* ---------------------------------------------------------------------- */

static inline int vfsub_mnt_want_write(struct vfsmount *mnt)
{
	int err;

	lockdep_off();
	err = mnt_want_write(mnt);
	lockdep_on();
	return err;
}

static inline void vfsub_mnt_drop_write(struct vfsmount *mnt)
{
	lockdep_off();
	mnt_drop_write(mnt);
	lockdep_on();
}

/* ---------------------------------------------------------------------- */

int vfsub_create(struct inode *dir, struct path *path, int mode,
		 bool want_excl);
int vfsub_link(struct dentry *src_dentry, struct inode *dir,
	       struct path *path, struct inode **delegated_inode);
int vfsub_mkdir(struct inode *dir, struct path *path, int mode);
int vfsub_rmdir(struct inode *dir, struct path *path);

/* ---------------------------------------------------------------------- */

int vfsub_iterate_dir(struct file *file, struct dir_context *ctx);

static inline loff_t vfsub_f_size_read(struct file *file)
{
	return i_size_read(file_inode(file));
}

static inline unsigned int vfsub_file_flags(struct file *file)
{
	unsigned int flags;

	spin_lock(&file->f_lock);
	flags = file->f_flags;
	spin_unlock(&file->f_lock);

	return flags;
}

/* ---------------------------------------------------------------------- */

static inline loff_t vfsub_llseek(struct file *file, loff_t offset, int origin)
{
	loff_t err;

	lockdep_off();
	err = vfs_llseek(file, offset, origin);
	lockdep_on();
	return err;
}

/* ---------------------------------------------------------------------- */

int vfsub_sio_notify_change(struct path *path, struct iattr *ia,
			    struct inode **delegated_inode);
int vfsub_notify_change(struct path *path, struct iattr *ia,
			struct inode **delegated_inode);
int vfsub_unlink(struct inode *dir, struct path *path,
		 struct inode **delegated_inode, int force);

#endif /* __KERNEL__ */
#endif /* __AUFS_VFSUB_H__ */
