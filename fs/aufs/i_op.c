/*
 * Copyright (C) 2005-2015 Junjiro R. Okajima
 */

/*
 * inode operations (except add/del/rename)
 */

#include <linux/device_cgroup.h>
#include <linux/fs_stack.h>
#include <linux/mm.h>
#include <linux/namei.h>
#include <linux/security.h>
#include <linux/uaccess.h>
#include "aufs.h"

static int h_permission(struct inode *h_inode, int mask,
			struct vfsmount *h_mnt, int brperm)
{
	int err;
	const unsigned char write_mask = !!(mask & (MAY_WRITE | MAY_APPEND));

	err = -EACCES;
	if ((write_mask && IS_IMMUTABLE(h_inode))
	    || ((mask & MAY_EXEC)
		&& S_ISREG(h_inode->i_mode)
		&& ((h_mnt->mnt_flags & MNT_NOEXEC)
		    || !(h_inode->i_mode & S_IXUGO))))
		goto out;

	/*
	 * - skip the lower fs test in the case of write to ro branch.
	 * - nfs dir permission write check is optimized, but a policy for
	 *   link/rename requires a real check.
	 */
	if ((write_mask && !au_br_writable(brperm))
	    || (au_test_nfs(h_inode->i_sb) && S_ISDIR(h_inode->i_mode)
		&& write_mask && !(mask & MAY_READ))
	    || !h_inode->i_op->permission) {
		/* AuLabel(generic_permission); */
		err = generic_permission(h_inode, mask);
	} else {
		/* AuLabel(h_inode->permission); */
		err = h_inode->i_op->permission(h_inode, mask);
		AuTraceErr(err);
	}

	if (!err)
		err = devcgroup_inode_permission(h_inode, mask);
	if (!err)
		err = security_inode_permission(h_inode, mask);

#if 0
	if (!err) {
		/* todo: do we need to call ima_path_check()? */
		struct path h_path = {
			.dentry	=
			.mnt	= h_mnt
		};
		err = ima_path_check(&h_path,
				     mask & (MAY_READ | MAY_WRITE | MAY_EXEC),
				     IMA_COUNT_LEAVE);
	}
#endif

out:
	return err;
}

static int aufs_permission(struct inode *inode, int mask)
{
	int err;
	aufs_bindex_t bindex, bend;
	const unsigned char isdir = !!S_ISDIR(inode->i_mode),
		write_mask = !!(mask & (MAY_WRITE | MAY_APPEND));
	struct inode *h_inode;
	struct super_block *sb;
	struct au_branch *br;

	/* todo: support rcu-walk? */
	if (mask & MAY_NOT_BLOCK)
		return -ECHILD;

	sb = inode->i_sb;
	si_read_lock(sb, AuLock_FLUSH);
	ii_read_lock_child(inode);
#if 0
	err = au_iigen_test(inode, au_sigen(sb));
	if (unlikely(err))
		goto out;
#endif

	if (!isdir
	    || write_mask) {
		err = -EBUSY;
		h_inode = au_h_iptr(inode, au_ibstart(inode));
		if (unlikely(!h_inode
			     || (h_inode->i_mode & S_IFMT)
			     != (inode->i_mode & S_IFMT)))
			goto out;

		err = 0;
		bindex = au_ibstart(inode);
		br = au_sbr(sb, bindex);
		err = h_permission(h_inode, mask, au_br_mnt(br), br->br_perm);
		if (write_mask
		    && !err
		    && !special_file(h_inode->i_mode)) {
			/* test whether the upper writable branch exists */
			err = -EROFS;
			for (; bindex >= 0; bindex--)
				if (!au_br_rdonly(au_sbr(sb, bindex))) {
					err = 0;
					break;
				}
		}
		goto out;
	}

	/* non-write to dir */
	err = 0;
	bend = au_ibend(inode);
	for (bindex = au_ibstart(inode); !err && bindex <= bend; bindex++) {
		h_inode = au_h_iptr(inode, bindex);
		if (h_inode) {
			err = -EBUSY;
			if (unlikely(!S_ISDIR(h_inode->i_mode)))
				break;

			br = au_sbr(sb, bindex);
			err = h_permission(h_inode, mask, au_br_mnt(br),
					   br->br_perm);
		}
	}

out:
	ii_read_unlock(inode);
	si_read_unlock(sb);
	return err;
}

/* ---------------------------------------------------------------------- */

static struct dentry *aufs_lookup(struct inode *dir, struct dentry *dentry,
				  unsigned int flags)
{
	struct dentry *ret, *parent;
	struct inode *inode;
	struct super_block *sb;
	int err, npositive;

	IMustLock(dir);

	/* todo: support rcu-walk? */
	ret = ERR_PTR(-ECHILD);
	if (flags & LOOKUP_RCU)
		goto out;

	ret = ERR_PTR(-ENAMETOOLONG);
	if (unlikely(dentry->d_name.len > AUFS_MAX_NAMELEN))
		goto out;

	sb = dir->i_sb;
	err = si_read_lock(sb, AuLock_FLUSH | AuLock_NOPLM);
	ret = ERR_PTR(err);
	if (unlikely(err))
		goto out;

	err = au_di_init(dentry);
	ret = ERR_PTR(err);
	if (unlikely(err))
		goto out_si;

	inode = NULL;
	npositive = 0; /* suppress a warning */
	parent = dentry->d_parent; /* dir inode is locked */
	di_read_lock_parent(parent, AuLock_IR);
	err = au_alive_dir(parent);
	if (!err)
		err = au_digen_test(parent, au_sigen(sb));
	if (!err) {
		npositive = au_lkup_dentry(dentry, au_dbstart(parent),
					   /*type*/0);
		err = npositive;
	}
	di_read_unlock(parent, AuLock_IR);
	ret = ERR_PTR(err);
	if (unlikely(err < 0))
		goto out_unlock;

	if (npositive) {
		inode = au_new_inode(dentry, /*must_new*/0);
		if (IS_ERR(inode)) {
			ret = (void *)inode;
			inode = NULL;
			goto out_unlock;
		}
	}

	if (inode)
		atomic_inc(&inode->i_count);
	ret = d_splice_alias(inode, dentry);
#if 0
	if (unlikely(d_need_lookup(dentry))) {
		spin_lock(&dentry->d_lock);
		dentry->d_flags &= ~DCACHE_NEED_LOOKUP;
		spin_unlock(&dentry->d_lock);
	} else
#endif
	if (inode) {
		if (!IS_ERR(ret)) {
			iput(inode);
			if (ret && ret != dentry)
				ii_write_unlock(inode);
		} else {
			ii_write_unlock(inode);
			iput(inode);
			inode = NULL;
		}
	}

out_unlock:
	di_write_unlock(dentry);
out_si:
	si_read_unlock(sb);
out:
	return ret;
}

/* ---------------------------------------------------------------------- */

void au_pin_hdir_unlock(struct au_pin *p)
{
	if (p->hdir)
		mutex_unlock(&p->hdir->hi_inode->i_mutex);
}

int au_pin_hdir_lock(struct au_pin *p)
{
	int err;

	err = 0;
	if (!p->hdir)
		goto out;

	/* even if an error happens later, keep this lock */
	mutex_lock_nested(&p->hdir->hi_inode->i_mutex, p->lsc_hi);

	err = -EBUSY;
	if (unlikely(p->hdir->hi_inode != p->h_parent->d_inode))
		goto out;

	err = 0;
	if (p->h_dentry)
		err = au_h_verify(p->h_dentry, p->udba, p->hdir->hi_inode,
				  p->h_parent, p->br);

out:
	return err;
}

int au_pin_hdir_relock(struct au_pin *p)
{
	int err, i;
	struct inode *h_i;
	struct dentry *h_d[] = {
		p->h_dentry,
		p->h_parent
	};

	err = au_pin_hdir_lock(p);
	if (unlikely(err))
		goto out;

	for (i = 0; !err && i < sizeof(h_d)/sizeof(*h_d); i++) {
		if (!h_d[i])
			continue;
		h_i = h_d[i]->d_inode;
		if (h_i)
			err = !h_i->i_nlink;
	}

out:
	return err;
}

void au_pin_hdir_set_owner(struct au_pin *p, struct task_struct *task)
{
#if defined(CONFIG_DEBUG_MUTEXES) || defined(CONFIG_SMP)
	p->hdir->hi_inode->i_mutex.owner = task;
#endif
}

void au_pin_hdir_acquire_nest(struct au_pin *p)
{
	if (p->hdir) {
		mutex_acquire_nest(&p->hdir->hi_inode->i_mutex.dep_map,
				   p->lsc_hi, 0, NULL, _RET_IP_);
		au_pin_hdir_set_owner(p, current);
	}
}

void au_pin_hdir_release(struct au_pin *p)
{
	if (p->hdir) {
		au_pin_hdir_set_owner(p, p->task);
		mutex_release(&p->hdir->hi_inode->i_mutex.dep_map, 1, _RET_IP_);
	}
}

struct dentry *au_pinned_h_parent(struct au_pin *pin)
{
	if (pin && pin->parent)
		return au_h_dptr(pin->parent, pin->bindex);
	return NULL;
}

void au_unpin(struct au_pin *p)
{
	if (p->hdir)
		au_pin_hdir_unlock(p);
	if (p->h_mnt && au_ftest_pin(p->flags, MNT_WRITE))
		vfsub_mnt_drop_write(p->h_mnt);
	if (!p->hdir)
		return;

	if (!au_ftest_pin(p->flags, DI_LOCKED))
		di_read_unlock(p->parent, AuLock_IR);
	iput(p->hdir->hi_inode);
	dput(p->parent);
	p->parent = NULL;
	p->hdir = NULL;
	p->h_mnt = NULL;
	/* do not clear p->task */
}

int au_do_pin(struct au_pin *p)
{
	int err;
	struct super_block *sb;
	struct inode *h_dir;

	err = 0;
	sb = p->dentry->d_sb;
	p->br = au_sbr(sb, p->bindex);
	if (IS_ROOT(p->dentry)) {
		if (au_ftest_pin(p->flags, MNT_WRITE)) {
			p->h_mnt = au_br_mnt(p->br);
			err = vfsub_mnt_want_write(p->h_mnt);
			if (unlikely(err)) {
				au_fclr_pin(p->flags, MNT_WRITE);
				goto out_err;
			}
		}
		goto out;
	}

	p->h_dentry = NULL;
	if (p->bindex <= au_dbend(p->dentry))
		p->h_dentry = au_h_dptr(p->dentry, p->bindex);

	p->parent = dget_parent(p->dentry);
	if (!au_ftest_pin(p->flags, DI_LOCKED))
		di_read_lock(p->parent, AuLock_IR, p->lsc_di);

	h_dir = NULL;
	p->h_parent = au_h_dptr(p->parent, p->bindex);
	p->hdir = au_hi(p->parent->d_inode, p->bindex);
	if (p->hdir)
		h_dir = p->hdir->hi_inode;

	/*
	 * udba case, or
	 * if DI_LOCKED is not set, then p->parent may be different
	 * and h_parent can be NULL.
	 */
	if (unlikely(!p->hdir || !h_dir || !p->h_parent)) {
		err = -EBUSY;
		if (!au_ftest_pin(p->flags, DI_LOCKED))
			di_read_unlock(p->parent, AuLock_IR);
		dput(p->parent);
		p->parent = NULL;
		goto out_err;
	}

	if (au_ftest_pin(p->flags, MNT_WRITE)) {
		p->h_mnt = au_br_mnt(p->br);
		err = vfsub_mnt_want_write(p->h_mnt);
		if (unlikely(err)) {
			au_fclr_pin(p->flags, MNT_WRITE);
			if (!au_ftest_pin(p->flags, DI_LOCKED))
				di_read_unlock(p->parent, AuLock_IR);
			dput(p->parent);
			p->parent = NULL;
			goto out_err;
		}
	}

	au_igrab(h_dir);
	err = au_pin_hdir_lock(p);
	if (!err)
		goto out; /* success */

	au_unpin(p);

out_err:
	pr_err("err %d\n", err);
	err = -EBUSY;
out:
	return err;
}

void au_pin_init(struct au_pin *p, struct dentry *dentry,
		 aufs_bindex_t bindex, int lsc_di, int lsc_hi,
		 unsigned int udba, unsigned char flags)
{
	p->dentry = dentry;
	p->udba = udba;
	p->lsc_di = lsc_di;
	p->lsc_hi = lsc_hi;
	p->flags = flags;
	p->bindex = bindex;

	p->parent = NULL;
	p->hdir = NULL;
	p->h_mnt = NULL;

	p->h_dentry = NULL;
	p->h_parent = NULL;
	p->br = NULL;
	p->task = current;
}

int au_pin(struct au_pin *pin, struct dentry *dentry, aufs_bindex_t bindex,
	   unsigned int udba, unsigned char flags)
{
	au_pin_init(pin, dentry, bindex, AuLsc_DI_PARENT, AuLsc_I_PARENT2,
		    udba, flags);
	return au_do_pin(pin);
}

/* ---------------------------------------------------------------------- */

static int h_readlink(struct dentry *dentry, int bindex, char __user *buf,
		      int bufsiz)
{
	int err;
	struct super_block *sb;
	struct dentry *h_dentry;

	err = -EINVAL;
	h_dentry = au_h_dptr(dentry, bindex);
	if (unlikely(!h_dentry->d_inode->i_op->readlink))
		goto out;

	err = security_inode_readlink(h_dentry);
	if (unlikely(err))
		goto out;

	sb = dentry->d_sb;
	if (!au_test_ro(sb, bindex, dentry->d_inode)) {
		vfsub_touch_atime(au_sbr_mnt(sb, bindex), h_dentry);
		fsstack_copy_attr_atime(dentry->d_inode, h_dentry->d_inode);
	}
	err = h_dentry->d_inode->i_op->readlink(h_dentry, buf, bufsiz);

out:
	return err;
}

static int aufs_readlink(struct dentry *dentry, char __user *buf, int bufsiz)
{
	int err;

	err = aufs_read_lock(dentry, AuLock_IR | AuLock_GEN);
	if (unlikely(err))
		goto out;
	err = au_d_hashed_positive(dentry);
	if (!err)
		err = h_readlink(dentry, au_dbstart(dentry), buf, bufsiz);
	aufs_read_unlock(dentry, AuLock_IR);

out:
	return err;
}

static void *aufs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	int err;
	mm_segment_t old_fs;
	union {
		char *k;
		char __user *u;
	} buf;

	err = -ENOMEM;
	buf.k = (void *)__get_free_page(GFP_NOFS);
	if (unlikely(!buf.k))
		goto out;

	err = aufs_read_lock(dentry, AuLock_IR | AuLock_GEN);
	if (unlikely(err))
		goto out_name;

	err = au_d_hashed_positive(dentry);
	if (!err) {
		old_fs = get_fs();
		set_fs(KERNEL_DS);
		err = h_readlink(dentry, au_dbstart(dentry), buf.u, PATH_MAX);
		set_fs(old_fs);
	}
	aufs_read_unlock(dentry, AuLock_IR);

	if (err >= 0) {
		buf.k[err] = 0;
		/* will be freed by put_link */
		nd_set_link(nd, buf.k);
		return NULL; /* success */
	}

out_name:
	free_page((unsigned long)buf.k);
out:
	AuTraceErr(err);
	return ERR_PTR(err);
}

static void aufs_put_link(struct dentry *dentry __maybe_unused,
			  struct nameidata *nd, void *cookie __maybe_unused)
{
	char *p;

	p = nd_get_link(nd);
	if (!IS_ERR_OR_NULL(p))
		free_page((unsigned long)p);
}

/* ---------------------------------------------------------------------- */

struct inode_operations aufs_symlink_iop = {
	.permission	= aufs_permission,

	.readlink	= aufs_readlink,
	.follow_link	= aufs_follow_link,
	.put_link	= aufs_put_link,
};

struct inode_operations aufs_dir_iop = {
	.lookup		= aufs_lookup,

	.permission	= aufs_permission
};

struct inode_operations aufs_iop = {
	.permission	= aufs_permission
};
