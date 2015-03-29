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
 * external inode number translation table and bitmap
 */

#include <linux/file.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include "aufs.h"

/* todo: unnecessary to support mmap_sem since kernel-space? */
ssize_t xino_fread(au_readf_t func, struct file *file, void *kbuf, size_t size,
		   loff_t *pos)
{
	ssize_t err;
	mm_segment_t oldfs;
	union {
		void *k;
		char __user *u;
	} buf;

	buf.k = kbuf;
	oldfs = get_fs();
	set_fs(KERNEL_DS);
	do {
		/* todo: signal_pending? */
		err = func(file, buf.u, size, pos);
	} while (err == -EAGAIN || err == -EINTR);
	set_fs(oldfs);

#if 0 /* reserved for future use */
	if (err > 0)
		fsnotify_access(file->f_path.dentry);
#endif

	return err;
}

/* ---------------------------------------------------------------------- */

static ssize_t do_xino_fwrite(au_writef_t func, struct file *file, void *kbuf,
			      size_t size, loff_t *pos)
{
	ssize_t err;
	mm_segment_t oldfs;
	union {
		void *k;
		const char __user *u;
	} buf;

	buf.k = kbuf;
	oldfs = get_fs();
	set_fs(KERNEL_DS);
	do {
		/* todo: signal_pending? */
		err = func(file, buf.u, size, pos);
	} while (err == -EAGAIN || err == -EINTR);
	set_fs(oldfs);

#if 0 /* reserved for future use */
	if (err > 0)
		fsnotify_modify(file->f_path.dentry);
#endif

	return err;
}

struct do_xino_fwrite_args {
	ssize_t *errp;
	au_writef_t func;
	struct file *file;
	void *buf;
	size_t size;
	loff_t *pos;
};

ssize_t xino_fwrite(au_writef_t func, struct file *file, void *buf, size_t size,
		    loff_t *pos)
{
	ssize_t err;

	lockdep_off();
	err = do_xino_fwrite(func, file, buf, size, pos);
	lockdep_on();

	return err;
}

/* ---------------------------------------------------------------------- */

/*
 * create a new xinofile at the same place/path as @base_file.
 */
struct file *au_xino_create2(struct file *base_file, struct file *copy_src)
{
	struct file *file;
	struct dentry *base, *parent;
	struct inode *dir, *delegated;
	struct qstr *name;
	struct path path;
	int err;

	base = base_file->f_path.dentry;
	parent = base->d_parent; /* dir inode is locked */
	dir = parent->d_inode;
	IMustLock(dir);

	file = ERR_PTR(-EINVAL);
	name = &base->d_name;
	path.dentry = vfsub_lookup_one_len(name->name, parent, name->len);
	if (IS_ERR(path.dentry)) {
		file = (void *)path.dentry;
		pr_err("%pd lookup err %ld\n",
		       base, PTR_ERR(path.dentry));
		goto out;
	}

	/* no need to mnt_want_write() since we call dentry_open() later */
	err = vfs_create(dir, path.dentry, S_IRUGO | S_IWUGO, NULL);
	if (unlikely(err)) {
		file = ERR_PTR(err);
		pr_err("%pd create err %d\n", base, err);
		goto out_dput;
	}

	path.mnt = base_file->f_path.mnt;
	file = vfsub_dentry_open(&path,
				 O_RDWR | O_CREAT | O_EXCL | O_LARGEFILE
				 /* | __FMODE_NONOTIFY */);
	if (IS_ERR(file)) {
		pr_err("%pd open err %ld\n", base, PTR_ERR(file));
		goto out_dput;
	}

	delegated = NULL;
	err = vfsub_unlink(dir, &file->f_path, &delegated, /*force*/0);
	if (unlikely(err == -EWOULDBLOCK)) {
		pr_warn("cannot retry for NFSv4 delegation"
			" for an internal unlink\n");
		iput(delegated);
	}
	if (unlikely(err)) {
		pr_err("%pd unlink err %d\n", base, err);
		goto out_fput;
	}

	if (copy_src) {
		/* no one can touch copy_src xino */
		/* err = au_copy_file(file, copy_src, vfsub_f_size_read(copy_src)); later */
		err = 0;
		if (unlikely(err)) {
			pr_err("%pd copy err %d\n", base, err);
			goto out_fput;
		}
	}
	goto out_dput; /* success */

out_fput:
	fput(file);
	file = ERR_PTR(err);
out_dput:
	dput(path.dentry);
out:
	return file;
}

struct au_xino_lock_dir {
	struct au_hinode *hdir;
	struct dentry *parent;
	struct mutex *mtx;
};

static void au_xino_lock_dir(struct super_block *sb, struct file *xino,
			     struct au_xino_lock_dir *ldir)
{
	aufs_bindex_t brid, bindex;

	ldir->hdir = NULL;
	bindex = -1;
	brid = au_xino_brid(sb);
	if (brid >= 0)
		bindex = au_br_index(sb, brid);
	if (bindex >= 0) {
		ldir->hdir = au_hi(sb->s_root->d_inode, bindex);
		mutex_lock_nested(&ldir->hdir->hi_inode->i_mutex,
				  AuLsc_I_PARENT);
	} else {
		ldir->parent = dget_parent(xino->f_path.dentry);
		ldir->mtx = &ldir->parent->d_inode->i_mutex;
		mutex_lock_nested(ldir->mtx, AuLsc_I_PARENT);
	}
}

static void au_xino_unlock_dir(struct au_xino_lock_dir *ldir)
{
	if (ldir->hdir)
		mutex_unlock(&ldir->hdir->hi_inode->i_mutex);
	else {
		mutex_unlock(ldir->mtx);
		dput(ldir->parent);
	}
}

/* ---------------------------------------------------------------------- */

static int au_xino_do_write(au_writef_t write, struct file *file,
			    ino_t h_ino, ino_t ino)
{
	loff_t pos;
	ssize_t sz;

	pos = h_ino;
	if (unlikely(au_loff_max / sizeof(ino) - 1 < pos)) {
		AuIOErr1("too large hi%lu\n", (unsigned long)h_ino);
		return -EFBIG;
	}
	pos *= sizeof(ino);
	sz = xino_fwrite(write, file, &ino, sizeof(ino), &pos);
	if (sz == sizeof(ino))
		return 0; /* success */

	AuIOErr("write failed (%zd)\n", sz);
	return -EIO;
}

/*
 * write @ino to the xinofile for the specified branch{@sb, @bindex}
 * at the position of @h_ino.
 * even if @ino is zero, it is written to the xinofile and means no entry.
 * if the size of the xino file on a specific filesystem exceeds the watermark,
 * try truncating it.
 */
int au_xino_write(struct super_block *sb, aufs_bindex_t bindex, ino_t h_ino,
		  ino_t ino)
{
	int err;
	unsigned int mnt_flags;
	struct au_branch *br;

	BUILD_BUG_ON(sizeof(long long) != sizeof(au_loff_max)
		     || ((loff_t)-1) > 0);
	SiMustAnyLock(sb);

	mnt_flags = au_mntflags(sb);
	if (!au_opt_test(mnt_flags, XINO))
		return 0;

	br = au_sbr(sb, bindex);
	err = au_xino_do_write(au_sbi(sb)->si_xwrite, br->br_xino.xi_file,
			       h_ino, ino);
	if (!err)
		return 0; /* success */

	AuIOErr("write failed (%d)\n", err);
	return -EIO;
}

/* ---------------------------------------------------------------------- */

/* aufs inode number bitmap */

static const int page_bits = (int)PAGE_SIZE * BITS_PER_BYTE;
static ino_t xib_calc_ino(unsigned long pindex, int bit)
{
	ino_t ino;

	AuDebugOn(bit < 0 || page_bits <= bit);
	ino = AUFS_FIRST_INO + pindex * page_bits + bit;
	return ino;
}

static void xib_calc_bit(ino_t ino, unsigned long *pindex, int *bit)
{
	AuDebugOn(ino < AUFS_FIRST_INO);
	ino -= AUFS_FIRST_INO;
	*pindex = ino / page_bits;
	*bit = ino % page_bits;
}

static int xib_pindex(struct super_block *sb, unsigned long pindex)
{
	int err;
	loff_t pos;
	ssize_t sz;
	struct au_sbinfo *sbinfo;
	struct file *xib;
	unsigned long *p;

	sbinfo = au_sbi(sb);
	MtxMustLock(&sbinfo->si_xib_mtx);
	AuDebugOn(pindex > ULONG_MAX / PAGE_SIZE
		  || !au_opt_test(sbinfo->si_mntflags, XINO));

	if (pindex == sbinfo->si_xib_last_pindex)
		return 0;

	xib = sbinfo->si_xib;
	p = sbinfo->si_xib_buf;
	pos = sbinfo->si_xib_last_pindex;
	pos *= PAGE_SIZE;
	sz = xino_fwrite(sbinfo->si_xwrite, xib, p, PAGE_SIZE, &pos);
	if (unlikely(sz != PAGE_SIZE))
		goto out;

	pos = pindex;
	pos *= PAGE_SIZE;
	if (vfsub_f_size_read(xib) >= pos + PAGE_SIZE)
		sz = xino_fread(sbinfo->si_xread, xib, p, PAGE_SIZE, &pos);
	else {
		memset(p, 0, PAGE_SIZE);
		sz = xino_fwrite(sbinfo->si_xwrite, xib, p, PAGE_SIZE, &pos);
	}
	if (sz == PAGE_SIZE) {
		sbinfo->si_xib_last_pindex = pindex;
		return 0; /* success */
	}

out:
	AuIOErr1("write failed (%zd)\n", sz);
	err = sz;
	if (sz >= 0)
		err = -EIO;
	return err;
}

/* ---------------------------------------------------------------------- */

static void au_xib_clear_bit(struct inode *inode)
{
	int err, bit;
	unsigned long pindex;
	struct super_block *sb;
	struct au_sbinfo *sbinfo;

	AuDebugOn(inode->i_nlink);

	sb = inode->i_sb;
	xib_calc_bit(inode->i_ino, &pindex, &bit);
	AuDebugOn(page_bits <= bit);
	sbinfo = au_sbi(sb);
	mutex_lock(&sbinfo->si_xib_mtx);
	err = xib_pindex(sb, pindex);
	if (!err) {
		clear_bit(bit, sbinfo->si_xib_buf);
		sbinfo->si_xib_next_bit = bit;
	}
	mutex_unlock(&sbinfo->si_xib_mtx);
}

/* for s_op->delete_inode() */
void au_xino_delete_inode(struct inode *inode, const int unlinked)
{
	int err;
	unsigned int mnt_flags;
	aufs_bindex_t bindex, bend, bi;
	struct au_iinfo *iinfo;
	struct super_block *sb;
	struct au_hinode *hi;
	struct inode *h_inode;
	struct au_branch *br;
	au_writef_t xwrite;

	sb = inode->i_sb;
	mnt_flags = au_mntflags(sb);
	if (!au_opt_test(mnt_flags, XINO)
	    || inode->i_ino == AUFS_ROOT_INO)
		return;

	if (unlinked)
		au_xib_clear_bit(inode);

	iinfo = au_ii(inode);
	if (!iinfo)
		return;

	bindex = iinfo->ii_bstart;
	if (bindex < 0)
		return;

	xwrite = au_sbi(sb)->si_xwrite;
	hi = iinfo->ii_hinode + bindex;
	bend = iinfo->ii_bend;
	for (; bindex <= bend; bindex++, hi++) {
		h_inode = hi->hi_inode;
		if (!h_inode
		    || (!unlinked && h_inode->i_nlink))
			continue;

		/* inode may not be revalidated */
		bi = au_br_index(sb, hi->hi_id);
		if (bi < 0)
			continue;

		br = au_sbr(sb, bi);
		err = au_xino_do_write(xwrite, br->br_xino.xi_file,
				       h_inode->i_ino, /*ino*/0);
	}
}

/* get an unused inode number from bitmap */
ino_t au_xino_new_ino(struct super_block *sb)
{
	ino_t ino;
	unsigned long *p, pindex, ul, pend;
	struct au_sbinfo *sbinfo;
	struct file *file;
	int free_bit, err;

	if (!au_opt_test(au_mntflags(sb), XINO))
		return iunique(sb, AUFS_FIRST_INO);

	sbinfo = au_sbi(sb);
	mutex_lock(&sbinfo->si_xib_mtx);
	p = sbinfo->si_xib_buf;
	free_bit = sbinfo->si_xib_next_bit;
	if (free_bit < page_bits && !test_bit(free_bit, p))
		goto out; /* success */
	free_bit = find_first_zero_bit(p, page_bits);
	if (free_bit < page_bits)
		goto out; /* success */

	pindex = sbinfo->si_xib_last_pindex;
	for (ul = pindex - 1; ul < ULONG_MAX; ul--) {
		err = xib_pindex(sb, ul);
		if (unlikely(err))
			goto out_err;
		free_bit = find_first_zero_bit(p, page_bits);
		if (free_bit < page_bits)
			goto out; /* success */
	}

	file = sbinfo->si_xib;
	pend = vfsub_f_size_read(file) / PAGE_SIZE;
	for (ul = pindex + 1; ul <= pend; ul++) {
		err = xib_pindex(sb, ul);
		if (unlikely(err))
			goto out_err;
		free_bit = find_first_zero_bit(p, page_bits);
		if (free_bit < page_bits)
			goto out; /* success */
	}
	BUG();

out:
	set_bit(free_bit, p);
	sbinfo->si_xib_next_bit = free_bit + 1;
	pindex = sbinfo->si_xib_last_pindex;
	mutex_unlock(&sbinfo->si_xib_mtx);
	ino = xib_calc_ino(pindex, free_bit);
	AuDbg("i%lu\n", (unsigned long)ino);
	return ino;
out_err:
	mutex_unlock(&sbinfo->si_xib_mtx);
	AuDbg("i0\n");
	return 0;
}

/*
 * read @ino from xinofile for the specified branch{@sb, @bindex}
 * at the position of @h_ino.
 * if @ino does not exist and @do_new is true, get new one.
 */
int au_xino_read(struct super_block *sb, aufs_bindex_t bindex, ino_t h_ino,
		 ino_t *ino)
{
	int err;
	ssize_t sz;
	loff_t pos;
	struct file *file;
	struct au_sbinfo *sbinfo;

	*ino = 0;
	if (!au_opt_test(au_mntflags(sb), XINO))
		return 0; /* no xino */

	err = 0;
	sbinfo = au_sbi(sb);
	pos = h_ino;
	if (unlikely(au_loff_max / sizeof(*ino) - 1 < pos)) {
		AuIOErr1("too large hi%lu\n", (unsigned long)h_ino);
		return -EFBIG;
	}
	pos *= sizeof(*ino);

	file = au_sbr(sb, bindex)->br_xino.xi_file;
	if (vfsub_f_size_read(file) < pos + sizeof(*ino))
		return 0; /* no ino */

	sz = xino_fread(sbinfo->si_xread, file, ino, sizeof(*ino), &pos);
	if (sz == sizeof(*ino))
		return 0; /* success */

	err = sz;
	if (unlikely(sz >= 0)) {
		err = -EIO;
		AuIOErr("xino read error (%zd)\n", sz);
	}

	return err;
}

/* ---------------------------------------------------------------------- */

/* create and set a new xino file */

struct file *au_xino_create(struct super_block *sb, char *fname, int silent)
{
	struct file *file;
	struct dentry *h_parent, *d;
	struct inode *h_dir;
	int err;

	/*
	 * at mount-time, and the xino file is the default path,
	 * hnotify is disabled so we have no notify events to ignore.
	 * when a user specified the xino, we cannot get au_hdir to be ignored.
	 */
	file = vfsub_filp_open(fname, O_RDWR | O_CREAT | O_EXCL | O_LARGEFILE
			       /* | __FMODE_NONOTIFY */,
			       S_IRUGO | S_IWUGO);
	if (IS_ERR(file)) {
		if (!silent)
			pr_err("open %s(%ld)\n", fname, PTR_ERR(file));
		return file;
	}

	/* keep file count */
	h_parent = dget_parent(file->f_path.dentry);
	h_dir = h_parent->d_inode;
	mutex_lock_nested(&h_dir->i_mutex, AuLsc_I_PARENT);
	/* mnt_want_write() is unnecessary here */
	/* no delegation since it is just created */
	err = vfsub_unlink(h_dir, &file->f_path, /*delegated*/NULL, /*force*/0);
	mutex_unlock(&h_dir->i_mutex);
	dput(h_parent);
	if (unlikely(err)) {
		if (!silent)
			pr_err("unlink %s(%d)\n", fname, err);
		goto out;
	}

	err = -EINVAL;
	d = file->f_path.dentry;
	if (unlikely(sb == d->d_sb)) {
		if (!silent)
			pr_err("%s must be outside\n", fname);
		goto out;
	}
	if (unlikely(au_test_fs_bad_xino(d->d_sb))) {
		if (!silent)
			pr_err("xino doesn't support %s(%s)\n",
			       fname, au_sbtype(d->d_sb));
		goto out;
	}
	return file; /* success */

out:
	fput(file);
	file = ERR_PTR(err);
	return file;
}

/* ---------------------------------------------------------------------- */

/*
 * initialize the xinofile for the specified branch @br
 * at the place/path where @base_file indicates.
 * test whether another branch is on the same filesystem or not,
 * if @do_test is true.
 */
int au_xino_br(struct super_block *sb, struct au_branch *br, ino_t h_ino,
	       struct file *base_file, int do_test)
{
	int err;
	ino_t ino;
	aufs_bindex_t bend, bindex;
	struct au_branch *shared_br, *b;
	struct file *file;
	struct super_block *tgt_sb;

	shared_br = NULL;
	bend = au_sbend(sb);
	if (do_test) {
		tgt_sb = au_br_sb(br);
		for (bindex = 0; bindex <= bend; bindex++) {
			b = au_sbr(sb, bindex);
			if (tgt_sb == au_br_sb(b)) {
				shared_br = b;
				break;
			}
		}
	}

	if (!shared_br || !shared_br->br_xino.xi_file) {
		struct au_xino_lock_dir ldir;

		au_xino_lock_dir(sb, base_file, &ldir);
		/* mnt_want_write() is unnecessary here */
		file = au_xino_create2(base_file, NULL);
		au_xino_unlock_dir(&ldir);
		err = PTR_ERR(file);
		if (IS_ERR(file))
			goto out;
		br->br_xino.xi_file = file;
	} else {
		br->br_xino.xi_file = shared_br->br_xino.xi_file;
		get_file(br->br_xino.xi_file);
	}

	ino = AUFS_ROOT_INO;
	err = au_xino_do_write(au_sbi(sb)->si_xwrite, br->br_xino.xi_file,
			       h_ino, ino);
	if (unlikely(err)) {
		fput(br->br_xino.xi_file);
		br->br_xino.xi_file = NULL;
	}

out:
	return err;
}
/* ---------------------------------------------------------------------- */

int au_xino_path(struct seq_file *seq, struct file *file)
{
	int err;

	err = au_seq_path(seq, &file->f_path);
	if (unlikely(err < 0))
		goto out;

	err = 0;
#define Deleted "\\040(deleted)"
	seq->count -= sizeof(Deleted) - 1;
	AuDebugOn(memcmp(seq->buf + seq->count, Deleted,
			 sizeof(Deleted) - 1));
#undef Deleted

out:
	return err;
}
