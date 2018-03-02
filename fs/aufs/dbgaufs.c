/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2005-2017 Junjiro R. Okajima
 */

/*
 * debugfs interface
 */

#include <linux/debugfs.h>
#include "aufs.h"

#ifndef CONFIG_SYSFS
#error DEBUG_FS depends upon SYSFS
#endif

static struct dentry *dbgaufs;
static const mode_t dbgaufs_mode = S_IRUSR | S_IRGRP | S_IROTH;

/* 20 is max digits length of ulong 64 */
struct dbgaufs_arg {
	int n;
	char a[20 * 4];
};

/*
 * common function for all XINO files
 */
static int dbgaufs_xi_release(struct inode *inode __maybe_unused,
			      struct file *file)
{
	kfree(file->private_data);
	return 0;
}

static int dbgaufs_xi_open(struct file *xf, struct file *file, int do_fcnt)
{
	int err;
	struct kstat st;
	struct dbgaufs_arg *p;

	err = -ENOMEM;
	p = kmalloc(sizeof(*p), GFP_NOFS);
	if (unlikely(!p))
		goto out;

	err = 0;
	p->n = 0;
	file->private_data = p;
	if (!xf)
		goto out;

	err = vfsub_getattr(&xf->f_path, &st);
	if (!err) {
		if (do_fcnt)
			p->n = snprintf
				(p->a, sizeof(p->a), "%ld, %llux%u %lld\n",
				 (long)file_count(xf), st.blocks, st.blksize,
				 (long long)st.size);
		else
			p->n = snprintf(p->a, sizeof(p->a), "%llux%u %lld\n",
					st.blocks, st.blksize,
					(long long)st.size);
		AuDebugOn(p->n >= sizeof(p->a));
	} else {
		p->n = snprintf(p->a, sizeof(p->a), "err %d\n", err);
		err = 0;
	}

out:
	return err;

}

static ssize_t dbgaufs_xi_read(struct file *file, char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct dbgaufs_arg *p;

	p = file->private_data;
	return simple_read_from_buffer(buf, count, ppos, p->a, p->n);
}

/* ---------------------------------------------------------------------- */

struct dbgaufs_plink_arg {
	int n;
	char a[];
};

static int dbgaufs_plink_release(struct inode *inode __maybe_unused,
				 struct file *file)
{
	free_page((unsigned long)file->private_data);
	return 0;
}

static int dbgaufs_plink_open(struct inode *inode, struct file *file)
{
	int err, i, limit;
	unsigned long n, sum;
	struct dbgaufs_plink_arg *p;
	struct au_sbinfo *sbinfo;
	struct super_block *sb;
	struct hlist_bl_head *hbl;

	err = -ENOMEM;
	p = (void *)get_zeroed_page(GFP_NOFS);
	if (unlikely(!p))
		goto out;

	err = -EFBIG;
	sbinfo = inode->i_private;
	sb = sbinfo->si_sb;
	si_noflush_read_lock(sb);
	if (au_opt_test(au_mntflags(sb), PLINK)) {
		limit = PAGE_SIZE - sizeof(p->n);

		/* the number of buckets */
		n = snprintf(p->a + p->n, limit, "%d\n", AuPlink_NHASH);
		p->n += n;
		limit -= n;

		sum = 0;
		for (i = 0, hbl = sbinfo->si_plink; i < AuPlink_NHASH;
		     i++, hbl++) {
			n = au_hbl_count(hbl);
			sum += n;

			n = snprintf(p->a + p->n, limit, "%lu ", n);
			p->n += n;
			limit -= n;
			if (unlikely(limit <= 0))
				goto out_free;
		}
		p->a[p->n - 1] = '\n';

		/* the sum of plinks */
		n = snprintf(p->a + p->n, limit, "%lu\n", sum);
		p->n += n;
		limit -= n;
		if (unlikely(limit <= 0))
			goto out_free;
	} else {
#define str "1\n0\n0\n"
		p->n = sizeof(str) - 1;
		strcpy(p->a, str);
#undef str
	}
	si_read_unlock(sb);

	err = 0;
	file->private_data = p;
	goto out; /* success */

out_free:
	free_page((unsigned long)p);
out:
	return err;
}

static ssize_t dbgaufs_plink_read(struct file *file, char __user *buf,
				  size_t count, loff_t *ppos)
{
	struct dbgaufs_plink_arg *p;

	p = file->private_data;
	return simple_read_from_buffer(buf, count, ppos, p->a, p->n);
}

static const struct file_operations dbgaufs_plink_fop = {
	.owner		= THIS_MODULE,
	.open		= dbgaufs_plink_open,
	.release	= dbgaufs_plink_release,
	.read		= dbgaufs_plink_read
};

/* ---------------------------------------------------------------------- */

static int dbgaufs_xib_open(struct inode *inode, struct file *file)
{
	int err;
	struct au_sbinfo *sbinfo;
	struct super_block *sb;

	sbinfo = inode->i_private;
	sb = sbinfo->si_sb;
	si_noflush_read_lock(sb);
	err = dbgaufs_xi_open(sbinfo->si_xib, file, /*do_fcnt*/0);
	si_read_unlock(sb);
	return err;
}

static const struct file_operations dbgaufs_xib_fop = {
	.owner		= THIS_MODULE,
	.open		= dbgaufs_xib_open,
	.release	= dbgaufs_xi_release,
	.read		= dbgaufs_xi_read
};

/* ---------------------------------------------------------------------- */

#define DbgaufsXi_PREFIX "xi"

static int dbgaufs_xino_open(struct inode *inode, struct file *file)
{
	int err;
	long l;
	struct au_sbinfo *sbinfo;
	struct super_block *sb;
	struct file *xf;
	struct qstr *name;

	err = -ENOENT;
	xf = NULL;
	name = &file->f_path.dentry->d_name;
	if (unlikely(name->len < sizeof(DbgaufsXi_PREFIX)
		     || memcmp(name->name, DbgaufsXi_PREFIX,
			       sizeof(DbgaufsXi_PREFIX) - 1)))
		goto out;
	err = kstrtol(name->name + sizeof(DbgaufsXi_PREFIX) - 1, 10, &l);
	if (unlikely(err))
		goto out;

	sbinfo = inode->i_private;
	sb = sbinfo->si_sb;
	si_noflush_read_lock(sb);
	if (l <= au_sbbot(sb)) {
		xf = au_sbr(sb, (aufs_bindex_t)l)->br_xino.xi_file;
		err = dbgaufs_xi_open(xf, file, /*do_fcnt*/1);
	} else
		err = -ENOENT;
	si_read_unlock(sb);

out:
	return err;
}

static const struct file_operations dbgaufs_xino_fop = {
	.owner		= THIS_MODULE,
	.open		= dbgaufs_xino_open,
	.release	= dbgaufs_xi_release,
	.read		= dbgaufs_xi_read
};

void dbgaufs_brs_del(struct super_block *sb, aufs_bindex_t bindex)
{
	aufs_bindex_t bbot;
	struct au_branch *br;
	struct au_xino_file *xi;

	if (!au_sbi(sb)->si_dbgaufs)
		return;

	bbot = au_sbbot(sb);
	for (; bindex <= bbot; bindex++) {
		br = au_sbr(sb, bindex);
		xi = &br->br_xino;
		/* debugfs acquires the parent i_mutex */
		lockdep_off();
		debugfs_remove(xi->xi_dbgaufs);
		lockdep_on();
		xi->xi_dbgaufs = NULL;
	}
}

void dbgaufs_brs_add(struct super_block *sb, aufs_bindex_t bindex)
{
	struct au_sbinfo *sbinfo;
	struct dentry *parent;
	struct au_branch *br;
	struct au_xino_file *xi;
	aufs_bindex_t bbot;
	char name[sizeof(DbgaufsXi_PREFIX) + 5]; /* "xi" bindex NULL */

	sbinfo = au_sbi(sb);
	parent = sbinfo->si_dbgaufs;
	if (!parent)
		return;

	bbot = au_sbbot(sb);
	for (; bindex <= bbot; bindex++) {
		snprintf(name, sizeof(name), DbgaufsXi_PREFIX "%d", bindex);
		br = au_sbr(sb, bindex);
		xi = &br->br_xino;
		AuDebugOn(xi->xi_dbgaufs);
		/* debugfs acquires the parent i_mutex */
		lockdep_off();
		xi->xi_dbgaufs = debugfs_create_file(name, dbgaufs_mode, parent,
						     sbinfo, &dbgaufs_xino_fop);
		lockdep_on();
		/* ignore an error */
		if (unlikely(!xi->xi_dbgaufs))
			AuWarn1("failed %s under debugfs\n", name);
	}
}

/* ---------------------------------------------------------------------- */

#ifdef CONFIG_AUFS_EXPORT
static int dbgaufs_xigen_open(struct inode *inode, struct file *file)
{
	int err;
	struct au_sbinfo *sbinfo;
	struct super_block *sb;

	sbinfo = inode->i_private;
	sb = sbinfo->si_sb;
	si_noflush_read_lock(sb);
	err = dbgaufs_xi_open(sbinfo->si_xigen, file, /*do_fcnt*/0);
	si_read_unlock(sb);
	return err;
}

static const struct file_operations dbgaufs_xigen_fop = {
	.owner		= THIS_MODULE,
	.open		= dbgaufs_xigen_open,
	.release	= dbgaufs_xi_release,
	.read		= dbgaufs_xi_read
};

static int dbgaufs_xigen_init(struct au_sbinfo *sbinfo)
{
	int err;

	/*
	 * This function is a dynamic '__init' function actually,
	 * so the tiny check for si_rwsem is unnecessary.
	 */
	/* AuRwMustWriteLock(&sbinfo->si_rwsem); */

	err = -EIO;
	sbinfo->si_dbgaufs_xigen = debugfs_create_file
		("xigen", dbgaufs_mode, sbinfo->si_dbgaufs, sbinfo,
		 &dbgaufs_xigen_fop);
	if (sbinfo->si_dbgaufs_xigen)
		err = 0;

	return err;
}
#else
static int dbgaufs_xigen_init(struct au_sbinfo *sbinfo)
{
	return 0;
}
#endif /* CONFIG_AUFS_EXPORT */

/* ---------------------------------------------------------------------- */

void dbgaufs_si_fin(struct au_sbinfo *sbinfo)
{
	/*
	 * This function is a dynamic '__fin' function actually,
	 * so the tiny check for si_rwsem is unnecessary.
	 */
	/* AuRwMustWriteLock(&sbinfo->si_rwsem); */

	debugfs_remove_recursive(sbinfo->si_dbgaufs);
	sbinfo->si_dbgaufs = NULL;
	kobject_put(&sbinfo->si_kobj);
}

int dbgaufs_si_init(struct au_sbinfo *sbinfo)
{
	int err;
	char name[SysaufsSiNameLen];

	/*
	 * This function is a dynamic '__init' function actually,
	 * so the tiny check for si_rwsem is unnecessary.
	 */
	/* AuRwMustWriteLock(&sbinfo->si_rwsem); */

	err = -ENOENT;
	if (!dbgaufs) {
		AuErr1("/debug/aufs is uninitialized\n");
		goto out;
	}

	err = -EIO;
	sysaufs_name(sbinfo, name);
	sbinfo->si_dbgaufs = debugfs_create_dir(name, dbgaufs);
	if (unlikely(!sbinfo->si_dbgaufs))
		goto out;
	kobject_get(&sbinfo->si_kobj);

	sbinfo->si_dbgaufs_xib = debugfs_create_file
		("xib", dbgaufs_mode, sbinfo->si_dbgaufs, sbinfo,
		 &dbgaufs_xib_fop);
	if (unlikely(!sbinfo->si_dbgaufs_xib))
		goto out_dir;

	sbinfo->si_dbgaufs_plink = debugfs_create_file
		("plink", dbgaufs_mode, sbinfo->si_dbgaufs, sbinfo,
		 &dbgaufs_plink_fop);
	if (unlikely(!sbinfo->si_dbgaufs_plink))
		goto out_dir;

	err = dbgaufs_xigen_init(sbinfo);
	if (!err)
		goto out; /* success */

out_dir:
	dbgaufs_si_fin(sbinfo);
out:
	return err;
}

/* ---------------------------------------------------------------------- */

void dbgaufs_fin(void)
{
	debugfs_remove(dbgaufs);
}

int __init dbgaufs_init(void)
{
	int err;

	err = -EIO;
	dbgaufs = debugfs_create_dir(AUFS_NAME, NULL);
	if (dbgaufs)
		err = 0;
	return err;
}
