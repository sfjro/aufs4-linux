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
