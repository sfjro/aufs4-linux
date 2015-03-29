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
 * copy-up functions, see wbr_policy.c for copy-down
 */

#include <linux/fs_stack.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include "aufs.h"

void au_cpup_attr_flags(struct inode *dst, unsigned int iflags)
{
	const unsigned int mask = S_DEAD | S_SWAPFILE | S_PRIVATE
		| S_NOATIME | S_NOCMTIME | S_AUTOMOUNT;

	BUILD_BUG_ON(sizeof(iflags) != sizeof(dst->i_flags));

	dst->i_flags |= iflags & ~mask;
	if (au_test_fs_notime(dst->i_sb))
		dst->i_flags |= S_NOATIME | S_NOCMTIME;
}

void au_cpup_attr_timesizes(struct inode *inode)
{
	struct inode *h_inode;

	h_inode = au_h_iptr(inode, au_ibstart(inode));
	fsstack_copy_attr_times(inode, h_inode);
	fsstack_copy_inode_size(inode, h_inode);
}

void au_cpup_attr_nlink(struct inode *inode, int force)
{
	struct inode *h_inode;
	struct super_block *sb;
	aufs_bindex_t bindex, bend;

	sb = inode->i_sb;
	bindex = au_ibstart(inode);
	h_inode = au_h_iptr(inode, bindex);
	if (!force
	    && !S_ISDIR(h_inode->i_mode)
	    && au_opt_test(au_mntflags(sb), PLINK)
	    && au_plink_test(inode))
		return;

	/*
	 * 0 can happen in revalidating.
	 * h_inode->i_mutex may not be held here, but it is harmless since once
	 * i_nlink reaches 0, it will never become positive except O_TMPFILE
	 * case.
	 * todo: O_TMPFILE+linkat(AT_SYMLINK_FOLLOW) bypassing aufs may cause
	 *	 the incorrect link count.
	 */
	set_nlink(inode, h_inode->i_nlink);

	/*
	 * fewer nlink makes find(1) noisy, but larger nlink doesn't.
	 * it may includes whplink directory.
	 */
	if (S_ISDIR(h_inode->i_mode)) {
		bend = au_ibend(inode);
		for (bindex++; bindex <= bend; bindex++) {
			h_inode = au_h_iptr(inode, bindex);
			if (h_inode)
				au_add_nlink(inode, h_inode);
		}
	}
}

void au_cpup_attr_changeable(struct inode *inode)
{
	struct inode *h_inode;

	h_inode = au_h_iptr(inode, au_ibstart(inode));
	inode->i_mode = h_inode->i_mode;
	inode->i_uid = h_inode->i_uid;
	inode->i_gid = h_inode->i_gid;
	au_cpup_attr_timesizes(inode);
	au_cpup_attr_flags(inode, h_inode->i_flags);
}

void au_cpup_igen(struct inode *inode, struct inode *h_inode)
{
	struct au_iinfo *iinfo = au_ii(inode);

	IiMustWriteLock(inode);

	iinfo->ii_higen = h_inode->i_generation;
	iinfo->ii_hsb1 = h_inode->i_sb;
}

void au_cpup_attr_all(struct inode *inode, int force)
{
	struct inode *h_inode;

	h_inode = au_h_iptr(inode, au_ibstart(inode));
	au_cpup_attr_changeable(inode);
	if (inode->i_nlink > 0)
		au_cpup_attr_nlink(inode, force);
	inode->i_rdev = h_inode->i_rdev;
	inode->i_blkbits = h_inode->i_blkbits;
	au_cpup_igen(inode, h_inode);
}

/* ---------------------------------------------------------------------- */

/* Note: dt_dentry and dt_h_dentry are not dget/dput-ed */

/* keep the timestamps of the parent dir when cpup */
void au_dtime_store(struct au_dtime *dt, struct dentry *dentry,
		    struct path *h_path)
{
	struct inode *h_inode;

	dt->dt_dentry = dentry;
	dt->dt_h_path = *h_path;
	h_inode = h_path->dentry->d_inode;
	dt->dt_atime = h_inode->i_atime;
	dt->dt_mtime = h_inode->i_mtime;
	/* smp_mb(); */
}

void au_dtime_revert(struct au_dtime *dt)
{
	struct iattr attr;
	int err;

	attr.ia_atime = dt->dt_atime;
	attr.ia_mtime = dt->dt_mtime;
	attr.ia_valid = ATTR_FORCE | ATTR_MTIME | ATTR_MTIME_SET
		| ATTR_ATIME | ATTR_ATIME_SET;

	/* no delegation since this is a directory */
	err = vfsub_notify_change(&dt->dt_h_path, &attr, /*delegated*/NULL);
	if (unlikely(err))
		pr_warn("restoring timestamps failed(%d). ignored\n", err);
}

/* ---------------------------------------------------------------------- */

/* internal use only */
struct au_cpup_reg_attr {
	int		valid;
	struct kstat	st;
	unsigned int	iflags; /* inode->i_flags */
};

static noinline_for_stack
int cpup_iattr(struct dentry *dst, aufs_bindex_t bindex, struct dentry *h_src,
	       struct au_cpup_reg_attr *h_src_attr)
{
	int err, sbits;
	struct iattr ia;
	struct path h_path;
	struct inode *h_isrc, *h_idst;
	struct kstat *h_st;

	h_path.dentry = au_h_dptr(dst, bindex);
	h_idst = h_path.dentry->d_inode;
	h_path.mnt = au_sbr_mnt(dst->d_sb, bindex);
	h_isrc = h_src->d_inode;
	ia.ia_valid = ATTR_FORCE | ATTR_UID | ATTR_GID
		| ATTR_ATIME | ATTR_MTIME
		| ATTR_ATIME_SET | ATTR_MTIME_SET;
	if (h_src_attr && h_src_attr->valid) {
		h_st = &h_src_attr->st;
		ia.ia_uid = h_st->uid;
		ia.ia_gid = h_st->gid;
		ia.ia_atime = h_st->atime;
		ia.ia_mtime = h_st->mtime;
		if (h_idst->i_mode != h_st->mode
		    && !S_ISLNK(h_idst->i_mode)) {
			ia.ia_valid |= ATTR_MODE;
			ia.ia_mode = h_st->mode;
		}
		sbits = !!(h_st->mode & (S_ISUID | S_ISGID));
		au_cpup_attr_flags(h_idst, h_src_attr->iflags);
	} else {
		ia.ia_uid = h_isrc->i_uid;
		ia.ia_gid = h_isrc->i_gid;
		ia.ia_atime = h_isrc->i_atime;
		ia.ia_mtime = h_isrc->i_mtime;
		if (h_idst->i_mode != h_isrc->i_mode
		    && !S_ISLNK(h_idst->i_mode)) {
			ia.ia_valid |= ATTR_MODE;
			ia.ia_mode = h_isrc->i_mode;
		}
		sbits = !!(h_isrc->i_mode & (S_ISUID | S_ISGID));
		au_cpup_attr_flags(h_idst, h_isrc->i_flags);
	}
	/* no delegation since it is just created */
	err = vfsub_notify_change(&h_path, &ia, /*delegated*/NULL);

	/* is this nfs only? */
	if (!err && sbits && au_test_nfs(h_path.dentry->d_sb)) {
		ia.ia_valid = ATTR_FORCE | ATTR_MODE;
		ia.ia_mode = h_isrc->i_mode;
		err = vfsub_notify_change(&h_path, &ia, /*delegated*/NULL);
	}

	return err;
}

/* ---------------------------------------------------------------------- */

static int au_do_copy_file(struct file *dst, struct file *src, loff_t len,
			   char *buf, unsigned long blksize)
{
	int err;
	size_t sz, rbytes, wbytes;
	unsigned char all_zero;
	char *p, *zp;
	struct mutex *h_mtx;
	/* reduce stack usage */
	struct iattr *ia;

	zp = page_address(ZERO_PAGE(0));
	if (unlikely(!zp))
		return -ENOMEM; /* possible? */

	err = 0;
	all_zero = 0;
	while (len) {
		AuDbg("len %lld\n", len);
		sz = blksize;
		if (len < blksize)
			sz = len;

		rbytes = 0;
		/* todo: signal_pending? */
		while (!rbytes || err == -EAGAIN || err == -EINTR) {
			rbytes = vfsub_read_k(src, buf, sz, &src->f_pos);
			err = rbytes;
		}
		if (unlikely(err < 0))
			break;

		all_zero = 0;
		if (len >= rbytes && rbytes == blksize)
			all_zero = !memcmp(buf, zp, rbytes);
		if (!all_zero) {
			wbytes = rbytes;
			p = buf;
			while (wbytes) {
				size_t b;

				b = vfsub_write_k(dst, p, wbytes, &dst->f_pos);
				err = b;
				/* todo: signal_pending? */
				if (unlikely(err == -EAGAIN || err == -EINTR))
					continue;
				if (unlikely(err < 0))
					break;
				wbytes -= b;
				p += b;
			}
			if (unlikely(err < 0))
				break;
		} else {
			loff_t res;

			AuLabel(hole);
			res = vfsub_llseek(dst, rbytes, SEEK_CUR);
			err = res;
			if (unlikely(res < 0))
				break;
		}
		len -= rbytes;
		err = 0;
	}

	/* the last block may be a hole */
	if (!err && all_zero) {
		AuLabel(last hole);

		err = 1;
		if (au_test_nfs(dst->f_path.dentry->d_sb)) {
			/* nfs requires this step to make last hole */
			/* is this only nfs? */
			do {
				/* todo: signal_pending? */
				err = vfsub_write_k(dst, "\0", 1, &dst->f_pos);
			} while (err == -EAGAIN || err == -EINTR);
			if (err == 1)
				dst->f_pos--;
		}

		if (err == 1) {
			ia = (void *)buf;
			ia->ia_size = dst->f_pos;
			ia->ia_valid = ATTR_SIZE | ATTR_FILE;
			ia->ia_file = dst;
			h_mtx = &file_inode(dst)->i_mutex;
			mutex_lock_nested(h_mtx, AuLsc_I_CHILD2);
			/* no delegation since it is just created */
			err = vfsub_notify_change(&dst->f_path, ia,
						  /*delegated*/NULL);
			mutex_unlock(h_mtx);
		}
	}

	return err;
}

int au_copy_file(struct file *dst, struct file *src, loff_t len)
{
	int err;
	unsigned long blksize;
	unsigned char do_kfree;
	char *buf;

	err = -ENOMEM;
	blksize = dst->f_path.dentry->d_sb->s_blocksize;
	if (!blksize || PAGE_SIZE < blksize)
		blksize = PAGE_SIZE;
	AuDbg("blksize %lu\n", blksize);
	do_kfree = (blksize != PAGE_SIZE && blksize >= sizeof(struct iattr *));
	if (do_kfree)
		buf = kmalloc(blksize, GFP_NOFS);
	else
		buf = (void *)__get_free_page(GFP_NOFS);
	if (unlikely(!buf))
		goto out;

	if (len > (1 << 22))
		AuDbg("copying a large file %lld\n", (long long)len);

	src->f_pos = 0;
	dst->f_pos = 0;
	err = au_do_copy_file(dst, src, len, buf, blksize);
	if (do_kfree)
		kfree(buf);
	else
		free_page((unsigned long)buf);

out:
	return err;
}

/*
 * to support a sparse file which is opened with O_APPEND,
 * we need to close the file.
 */
static int au_cp_regular(struct au_cp_generic *cpg)
{
	int err, i;
	enum { SRC, DST };
	struct {
		aufs_bindex_t bindex;
		unsigned int flags;
		struct dentry *dentry;
		struct file *file;
		void *label;
	} *f, file[] = {
		{
			.bindex = cpg->bsrc,
			.flags = O_RDONLY | O_NOATIME | O_LARGEFILE,
			.label = &&out
		},
		{
			.bindex = cpg->bdst,
			.flags = O_WRONLY | O_NOATIME | O_LARGEFILE,
			.label = &&out_src
		}
	};
	struct super_block *sb;

	/* bsrc branch can be ro/rw. */
	sb = cpg->dentry->d_sb;
	f = file;
	for (i = 0; i < 2; i++, f++) {
		f->dentry = au_h_dptr(cpg->dentry, f->bindex);
		f->file = au_h_open(cpg->dentry, f->bindex, f->flags,
				    /*file*/NULL);
		err = PTR_ERR(f->file);
		if (IS_ERR(f->file))
			goto *f->label;
	}

	/* try stopping to update while we copyup */
	IMustLock(file[SRC].dentry->d_inode);
	err = au_copy_file(file[DST].file, file[SRC].file, cpg->len);

	fput(file[DST].file);
	au_sbr_put(sb, file[DST].bindex);

out_src:
	fput(file[SRC].file);
	au_sbr_put(sb, file[SRC].bindex);
out:
	return err;
}

static int au_do_cpup_regular(struct au_cp_generic *cpg,
			      struct au_cpup_reg_attr *h_src_attr)
{
	int err, rerr;
	loff_t l;
	struct path h_path;
	struct inode *h_src_inode, *h_dst_inode;

	err = 0;
	h_src_inode = au_h_iptr(cpg->dentry->d_inode, cpg->bsrc);
	l = i_size_read(h_src_inode);
	if (cpg->len == -1 || l < cpg->len)
		cpg->len = l;
	if (cpg->len) {
		/* try stopping to update while we are referencing */
		mutex_lock_nested(&h_src_inode->i_mutex, AuLsc_I_CHILD);
		au_pin_hdir_unlock(cpg->pin);

		h_path.dentry = au_h_dptr(cpg->dentry, cpg->bsrc);
		h_path.mnt = au_sbr_mnt(cpg->dentry->d_sb, cpg->bsrc);
		h_src_attr->iflags = h_src_inode->i_flags;
		err = vfs_getattr(&h_path, &h_src_attr->st);
		if (unlikely(err)) {
			mutex_unlock(&h_src_inode->i_mutex);
			goto out;
		}
		h_src_attr->valid = 1;
		err = au_cp_regular(cpg);
		mutex_unlock(&h_src_inode->i_mutex);
		rerr = au_pin_hdir_relock(cpg->pin);
		if (!err && rerr)
			err = rerr;
	}
	if (!err && (h_src_inode->i_state & I_LINKABLE)) {
		h_path.dentry = au_h_dptr(cpg->dentry, cpg->bdst);
		h_dst_inode = h_path.dentry->d_inode;
		spin_lock(&h_dst_inode->i_lock);
		h_dst_inode->i_state |= I_LINKABLE;
		spin_unlock(&h_dst_inode->i_lock);
	}

out:
	return err;
}

static int au_do_cpup_symlink(struct path *h_path, struct dentry *h_src,
			      struct inode *h_dir)
{
	int err, symlen;
	mm_segment_t old_fs;
	union {
		char *k;
		char __user *u;
	} sym;

	err = -ENOSYS;
	if (unlikely(!h_src->d_inode->i_op->readlink))
		goto out;

	err = -ENOMEM;
	sym.k = (void *)__get_free_page(GFP_NOFS);
	if (unlikely(!sym.k))
		goto out;

	/* unnecessary to support mmap_sem since symlink is not mmap-able */
	old_fs = get_fs();
	set_fs(KERNEL_DS);
	symlen = h_src->d_inode->i_op->readlink(h_src, sym.u, PATH_MAX);
	err = symlen;
	set_fs(old_fs);

	if (symlen > 0) {
		sym.k[symlen] = 0;
		err = vfsub_symlink(h_dir, h_path, sym.k);
	}
	free_page((unsigned long)sym.k);

out:
	return err;
}

static noinline_for_stack
int cpup_entry(struct au_cp_generic *cpg, struct dentry *dst_parent,
	       struct au_cpup_reg_attr *h_src_attr)
{
	int err;
	umode_t mode;
	unsigned int mnt_flags;
	unsigned char isdir, isreg, force;
	const unsigned char do_dt = !!au_ftest_cpup(cpg->flags, DTIME);
	struct au_dtime dt;
	struct path h_path;
	struct dentry *h_src, *h_dst, *h_parent;
	struct inode *h_inode, *h_dir;
	struct super_block *sb;

	/* bsrc branch can be ro/rw. */
	h_src = au_h_dptr(cpg->dentry, cpg->bsrc);
	h_inode = h_src->d_inode;
	AuDebugOn(h_inode != au_h_iptr(cpg->dentry->d_inode, cpg->bsrc));

	/* try stopping to be referenced while we are creating */
	h_dst = au_h_dptr(cpg->dentry, cpg->bdst);
	if (au_ftest_cpup(cpg->flags, RENAME))
		AuDebugOn(strncmp(h_dst->d_name.name, AUFS_WH_PFX,
				  AUFS_WH_PFX_LEN));
	h_parent = h_dst->d_parent; /* dir inode is locked */
	h_dir = h_parent->d_inode;
	IMustLock(h_dir);
	AuDebugOn(h_parent != h_dst->d_parent);

	sb = cpg->dentry->d_sb;
	h_path.mnt = au_sbr_mnt(sb, cpg->bdst);
	if (do_dt) {
		h_path.dentry = h_parent;
		au_dtime_store(&dt, dst_parent, &h_path);
	}
	h_path.dentry = h_dst;

	isreg = 0;
	isdir = 0;
	mode = h_inode->i_mode;
	switch (mode & S_IFMT) {
	case S_IFREG:
		isreg = 1;
		err = vfsub_create(h_dir, &h_path, mode | S_IWUSR,
				   /*want_excl*/true);
		if (!err)
			err = au_do_cpup_regular(cpg, h_src_attr);
		break;
	case S_IFDIR:
		isdir = 1;
		err = vfsub_mkdir(h_dir, &h_path, mode);
		if (!err) {
			/*
			 * strange behaviour from the users view,
			 * particularry setattr case
			 */
			if (au_ibstart(dst_parent->d_inode) == cpg->bdst)
				au_cpup_attr_nlink(dst_parent->d_inode,
						   /*force*/1);
			au_cpup_attr_nlink(cpg->dentry->d_inode, /*force*/1);
		}
		break;
	case S_IFLNK:
		err = au_do_cpup_symlink(&h_path, h_src, h_dir);
		break;
	case S_IFCHR:
	case S_IFBLK:
		AuDebugOn(!capable(CAP_MKNOD));
		/*FALLTHROUGH*/
	case S_IFIFO:
	case S_IFSOCK:
		err = vfsub_mknod(h_dir, &h_path, mode, h_inode->i_rdev);
		break;
	default:
		AuIOErr("Unknown inode type 0%o\n", mode);
		err = -EIO;
	}

	mnt_flags = au_mntflags(sb);
	if (!isdir
	    && au_opt_test(mnt_flags, XINO)
	    && (h_inode->i_nlink == 1
		|| (h_inode->i_state & I_LINKABLE))
	    /* todo: unnecessary? */
	    /* && cpg->dentry->d_inode->i_nlink == 1 */
	    && cpg->bdst < cpg->bsrc
	    && !au_ftest_cpup(cpg->flags, KEEPLINO))
		au_xino_write(sb, cpg->bsrc, h_inode->i_ino, /*ino*/0);
		/* ignore this error */

	if (!err) {
		force = 0;
		if (isreg) {
			force = !!cpg->len;
			if (cpg->len == -1)
				force = !!i_size_read(h_inode);
		}
	}

	if (do_dt)
		au_dtime_revert(&dt);
	return err;
}

static int au_do_ren_after_cpup(struct au_cp_generic *cpg, struct path *h_path)
{
	int err;
	struct dentry *dentry, *h_dentry, *h_parent;
	struct inode *h_dir;
	aufs_bindex_t bdst;

	dentry = cpg->dentry;
	bdst = cpg->bdst;
	h_dentry = au_h_dptr(dentry, bdst);
	dget(h_dentry);
	au_set_h_dptr(dentry, bdst, NULL);
	err = au_lkup_neg(dentry, bdst, /*wh*/0);
	if (!err)
		h_path->dentry = dget(au_h_dptr(dentry, bdst));
	au_set_h_dptr(dentry, bdst, h_dentry);
	if (unlikely(err))
		goto out;

	h_parent = h_dentry->d_parent; /* dir inode is locked */
	h_dir = h_parent->d_inode;
	IMustLock(h_dir);
	AuDbg("%pd %pd\n", h_dentry, h_path->dentry);
	/* no delegation since it is just created */
	err = vfsub_rename(h_dir, h_dentry, h_dir, h_path, /*delegated*/NULL);
	dput(h_path->dentry);

out:
	return err;
}

/*
 * copyup the @dentry from @bsrc to @bdst.
 * the caller must set the both of lower dentries.
 * @len is for truncating when it is -1 copyup the entire file.
 * in link/rename cases, @dst_parent may be different from the real one.
 * basic->bsrc can be larger than basic->bdst.
 */
static int au_cpup_single(struct au_cp_generic *cpg, struct dentry *dst_parent)
{
	int err, rerr;
	aufs_bindex_t old_ibstart;
	unsigned char isdir, plink;
	struct dentry *h_src, *h_dst, *h_parent;
	struct inode *dst_inode, *h_dir, *inode, *delegated;
	struct super_block *sb;
	struct au_branch *br;
	/* to reuduce stack size */
	struct {
		struct au_dtime dt;
		struct path h_path;
		struct au_cpup_reg_attr h_src_attr;
	} *a;

	err = -ENOMEM;
	a = kmalloc(sizeof(*a), GFP_NOFS);
	if (unlikely(!a))
		goto out;
	a->h_src_attr.valid = 0;

	sb = cpg->dentry->d_sb;
	br = au_sbr(sb, cpg->bdst);
	a->h_path.mnt = au_br_mnt(br);
	h_dst = au_h_dptr(cpg->dentry, cpg->bdst);
	h_parent = h_dst->d_parent; /* dir inode is locked */
	h_dir = h_parent->d_inode;
	IMustLock(h_dir);

	h_src = au_h_dptr(cpg->dentry, cpg->bsrc);
	inode = cpg->dentry->d_inode;

	if (!dst_parent)
		dst_parent = dget_parent(cpg->dentry);
	else
		dget(dst_parent);

	plink = !!au_opt_test(au_mntflags(sb), PLINK);
	dst_inode = au_h_iptr(inode, cpg->bdst);
	if (dst_inode) {
		if (unlikely(!plink)) {
			err = -EIO;
			AuIOErr("hi%lu(i%lu) exists on b%d "
				"but plink is disabled\n",
				dst_inode->i_ino, inode->i_ino, cpg->bdst);
			goto out_parent;
		}

		if (dst_inode->i_nlink) {
			const int do_dt = au_ftest_cpup(cpg->flags, DTIME);

			h_src = au_plink_lkup(inode, cpg->bdst);
			err = PTR_ERR(h_src);
			if (IS_ERR(h_src))
				goto out_parent;
			if (unlikely(!h_src->d_inode)) {
				err = -EIO;
				AuIOErr("i%lu exists on a upper branch "
					"but not pseudo-linked\n",
					inode->i_ino);
				dput(h_src);
				goto out_parent;
			}

			if (do_dt) {
				a->h_path.dentry = h_parent;
				au_dtime_store(&a->dt, dst_parent, &a->h_path);
			}

			a->h_path.dentry = h_dst;
			delegated = NULL;
			err = vfsub_link(h_src, h_dir, &a->h_path, &delegated);
			if (!err && au_ftest_cpup(cpg->flags, RENAME))
				err = au_do_ren_after_cpup(cpg, &a->h_path);
			if (do_dt)
				au_dtime_revert(&a->dt);
			if (unlikely(err == -EWOULDBLOCK)) {
				pr_warn("cannot retry for NFSv4 delegation"
					" for an internal link\n");
				iput(delegated);
			}
			dput(h_src);
			goto out_parent;
		} else
			/* todo: cpup_wh_file? */
			/* udba work */
			au_update_ibrange(inode, /*do_put_zero*/1);
	}

	isdir = S_ISDIR(inode->i_mode);
	old_ibstart = au_ibstart(inode);
	err = cpup_entry(cpg, dst_parent, &a->h_src_attr);
	if (unlikely(err))
		goto out_rev;
	dst_inode = h_dst->d_inode;
	mutex_lock_nested(&dst_inode->i_mutex, AuLsc_I_CHILD2);
	/* todo: necessary? */
	/* au_pin_hdir_unlock(cpg->pin); */

	err = cpup_iattr(cpg->dentry, cpg->bdst, h_src, &a->h_src_attr);
	if (unlikely(err)) {
		/* todo: necessary? */
		/* au_pin_hdir_relock(cpg->pin); */ /* ignore an error */
		mutex_unlock(&dst_inode->i_mutex);
		goto out_rev;
	}

	if (cpg->bdst < old_ibstart) {
		au_set_ibstart(inode, cpg->bdst);
	} else
		au_set_ibend(inode, cpg->bdst);
	au_set_h_iptr(inode, cpg->bdst, au_igrab(dst_inode),
		      au_hi_flags(inode, isdir));

	/* todo: necessary? */
	/* err = au_pin_hdir_relock(cpg->pin); */
	mutex_unlock(&dst_inode->i_mutex);
	if (unlikely(err))
		goto out_rev;

	if (!isdir
	    && (h_src->d_inode->i_nlink > 1
		|| h_src->d_inode->i_state & I_LINKABLE)
	    && plink)
		au_plink_append(inode, cpg->bdst, h_dst);

	if (au_ftest_cpup(cpg->flags, RENAME)) {
		a->h_path.dentry = h_dst;
		err = au_do_ren_after_cpup(cpg, &a->h_path);
	}
	if (!err)
		goto out_parent; /* success */

	/* revert */
out_rev:
	a->h_path.dentry = h_parent;
	au_dtime_store(&a->dt, dst_parent, &a->h_path);
	a->h_path.dentry = h_dst;
	rerr = 0;
	if (h_dst->d_inode) {
		if (!isdir) {
			/* no delegation since it is just created */
			rerr = vfsub_unlink(h_dir, &a->h_path,
					    /*delegated*/NULL, /*force*/0);
		} else
			rerr = vfsub_rmdir(h_dir, &a->h_path);
	}
	au_dtime_revert(&a->dt);
	if (rerr) {
		AuIOErr("failed removing broken entry(%d, %d)\n", err, rerr);
		err = -EIO;
	}
out_parent:
	dput(dst_parent);
	kfree(a);
out:
	return err;
}

#if 0 /* reserved */
struct au_cpup_single_args {
	int *errp;
	struct au_cp_generic *cpg;
	struct dentry *dst_parent;
};

static void au_call_cpup_single(void *args)
{
	struct au_cpup_single_args *a = args;

	au_pin_hdir_acquire_nest(a->cpg->pin);
	*a->errp = au_cpup_single(a->cpg, a->dst_parent);
	au_pin_hdir_release(a->cpg->pin);
}
#endif

#if 0 /* reserved */
int au_sio_cpup_single(struct au_cp_generic *cpg, struct dentry *dst_parent)
{
	int err, wkq_err;
	struct dentry *h_dentry;

	h_dentry = au_h_dptr(cpg->dentry, cpg->bsrc);
	if (!au_cpup_sio_test(pin, h_dentry->d_inode->i_mode))
		err = au_cpup_single(cpg, dst_parent);
	else {
		struct au_cpup_single_args args = {
			.errp		= &err,
			.cpg		= cpg,
			.dst_parent	= dst_parent
		};
		wkq_err = au_wkq_wait(au_call_cpup_single, &args);
		if (unlikely(wkq_err))
			err = wkq_err;
	}

	return err;
}
#endif
