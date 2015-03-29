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
 * mount and super_block operations
 */

#include <linux/mm.h>
#include <linux/seq_file.h>
#include "aufs.h"

/*
 * super_operations
 */
static struct inode *aufs_alloc_inode(struct super_block *sb __maybe_unused)
{
	struct au_icntnr *c;

	c = au_cache_alloc_icntnr();
	if (c) {
		au_icntnr_init(c);
		c->vfs_inode.i_version = 1; /* sigen(sb); */
		c->iinfo.ii_hinode = NULL;
		return &c->vfs_inode;
	}
	return NULL;
}

static void aufs_destroy_inode_cb(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);

	INIT_HLIST_HEAD(&inode->i_dentry);
	au_cache_free_icntnr(container_of(inode, struct au_icntnr, vfs_inode));
}

static void aufs_destroy_inode(struct inode *inode)
{
	au_iinfo_fin(inode);
	call_rcu(&inode->i_rcu, aufs_destroy_inode_cb);
}

struct inode *au_iget_locked(struct super_block *sb, ino_t ino)
{
	struct inode *inode;
	int err;

	inode = iget_locked(sb, ino);
	if (unlikely(!inode)) {
		inode = ERR_PTR(-ENOMEM);
		goto out;
	}
	if (!(inode->i_state & I_NEW))
		goto out;

	err = au_iinfo_init(inode);
	if (!err)
		inode->i_version++;
	else {
		iget_failed(inode);
		inode = ERR_PTR(err);
	}

out:
	/* never return NULL */
	AuDebugOn(!inode);
	AuTraceErrPtr(inode);
	return inode;
}

/* lock free root dinfo */
static int au_show_brs(struct seq_file *seq, struct super_block *sb)
{
	int err;
	aufs_bindex_t bindex, bend;
	struct path path;
	struct au_hdentry *hdp;
	struct au_branch *br;
	au_br_perm_str_t perm;

	err = 0;
	bend = au_sbend(sb);
	hdp = au_di(sb->s_root)->di_hdentry;
	for (bindex = 0; !err && bindex <= bend; bindex++) {
		br = au_sbr(sb, bindex);
		path.mnt = au_br_mnt(br);
		path.dentry = hdp[bindex].hd_dentry;
		err = au_seq_path(seq, &path);
		if (err > 0) {
			au_optstr_br_perm(&perm, br->br_perm);
			err = seq_printf(seq, "=%s", perm.a);
			if (err == -1)
				err = -E2BIG;
		}
		if (!err && bindex != bend)
			err = seq_putc(seq, ':');
	}

	return err;
}

static void au_show_wbr_create(struct seq_file *m, int v,
			       struct au_sbinfo *sbinfo)
{
	const char *pat;

	AuRwMustAnyLock(&sbinfo->si_rwsem);

	seq_puts(m, ",create=");
	pat = au_optstr_wbr_create(v);
	switch (v) {
	case AuWbrCreate_TDP:
	case AuWbrCreate_RR:
	case AuWbrCreate_MFS:
	case AuWbrCreate_PMFS:
		seq_puts(m, pat);
		break;
	case AuWbrCreate_MFSV:
		seq_printf(m, /*pat*/"mfs:%lu",
			   jiffies_to_msecs(sbinfo->si_wbr_mfs.mfs_expire)
			   / MSEC_PER_SEC);
		break;
	case AuWbrCreate_PMFSV:
		seq_printf(m, /*pat*/"pmfs:%lu",
			   jiffies_to_msecs(sbinfo->si_wbr_mfs.mfs_expire)
			   / MSEC_PER_SEC);
		break;
	case AuWbrCreate_MFSRR:
		seq_printf(m, /*pat*/"mfsrr:%llu",
			   sbinfo->si_wbr_mfs.mfsrr_watermark);
		break;
	case AuWbrCreate_MFSRRV:
		seq_printf(m, /*pat*/"mfsrr:%llu:%lu",
			   sbinfo->si_wbr_mfs.mfsrr_watermark,
			   jiffies_to_msecs(sbinfo->si_wbr_mfs.mfs_expire)
			   / MSEC_PER_SEC);
		break;
	case AuWbrCreate_PMFSRR:
		seq_printf(m, /*pat*/"pmfsrr:%llu",
			   sbinfo->si_wbr_mfs.mfsrr_watermark);
		break;
	case AuWbrCreate_PMFSRRV:
		seq_printf(m, /*pat*/"pmfsrr:%llu:%lu",
			   sbinfo->si_wbr_mfs.mfsrr_watermark,
			   jiffies_to_msecs(sbinfo->si_wbr_mfs.mfs_expire)
			   / MSEC_PER_SEC);
		break;
	}
}

static int au_show_xino(struct seq_file *seq, struct super_block *sb)
{
#ifdef CONFIG_SYSFS
	return 0;
#else
	int err;
	const int len = sizeof(AUFS_XINO_FNAME) - 1;
	aufs_bindex_t bindex, brid;
	struct qstr *name;
	struct file *f;
	struct dentry *d, *h_root;
	struct au_hdentry *hdp;

	AuRwMustAnyLock(&sbinfo->si_rwsem);

	err = 0;
	f = au_sbi(sb)->si_xib;
	if (!f)
		goto out;

	/* stop printing the default xino path on the first writable branch */
	h_root = NULL;
	brid = au_xino_brid(sb);
	if (brid >= 0) {
		bindex = au_br_index(sb, brid);
		hdp = au_di(sb->s_root)->di_hdentry;
		h_root = hdp[0 + bindex].hd_dentry;
	}
	d = f->f_path.dentry;
	name = &d->d_name;
	/* safe ->d_parent because the file is unlinked */
	if (d->d_parent == h_root
	    && name->len == len
	    && !memcmp(name->name, AUFS_XINO_FNAME, len))
		goto out;

	seq_puts(seq, ",xino=");
	err = au_xino_path(seq, f);

out:
	return err;
#endif
}

/* ---------------------------------------------------------------------- */

/* final actions when unmounting a file system */
static void aufs_put_super(struct super_block *sb)
{
	struct au_sbinfo *sbinfo;

	sbinfo = au_sbi(sb);
	if (!sbinfo)
		return;

	kobject_put(&sbinfo->si_kobj);
}

/* ---------------------------------------------------------------------- */

void au_array_free(void *array)
{
	if (array) {
		if (!is_vmalloc_addr(array))
			kfree(array);
		else
			vfree(array);
	}
}

void *au_array_alloc(unsigned long long *hint, au_arraycb_t cb, void *arg)
{
	void *array;
	unsigned long long n, sz;

	array = NULL;
	n = 0;
	if (!*hint)
		goto out;

	if (*hint > ULLONG_MAX / sizeof(array)) {
		array = ERR_PTR(-EMFILE);
		pr_err("hint %llu\n", *hint);
		goto out;
	}

	sz = sizeof(array) * *hint;
	array = kzalloc(sz, GFP_NOFS);
	if (unlikely(!array))
		array = vzalloc(sz);
	if (unlikely(!array)) {
		array = ERR_PTR(-ENOMEM);
		goto out;
	}

	n = cb(array, *hint, arg);
	AuDebugOn(n > *hint);

out:
	*hint = n;
	return array;
}

static unsigned long long au_iarray_cb(void *a,
				       unsigned long long max __maybe_unused,
				       void *arg)
{
	unsigned long long n;
	struct inode **p, *inode;
	struct list_head *head;

	n = 0;
	p = a;
	head = arg;
	spin_lock(&inode_sb_list_lock);
	list_for_each_entry(inode, head, i_sb_list) {
		if (!is_bad_inode(inode)
		    && au_ii(inode)->ii_bstart >= 0) {
			spin_lock(&inode->i_lock);
			if (atomic_read(&inode->i_count)) {
				au_igrab(inode);
				*p++ = inode;
				n++;
				AuDebugOn(n > max);
			}
			spin_unlock(&inode->i_lock);
		}
	}
	spin_unlock(&inode_sb_list_lock);

	return n;
}

struct inode **au_iarray_alloc(struct super_block *sb, unsigned long long *max)
{
	*max = atomic_long_read(&au_sbi(sb)->si_ninodes);
	return au_array_alloc(max, au_iarray_cb, &sb->s_inodes);
}

void au_iarray_free(struct inode **a, unsigned long long max)
{
	unsigned long long ull;

	for (ull = 0; ull < max; ull++)
		iput(a[ull]);
	au_array_free(a);
}

/* ---------------------------------------------------------------------- */

/*
 * refresh dentry and inode at remount time.
 */
/* todo: consolidate with simple_reval_dpath() and au_reval_for_attr() */
static int au_do_refresh(struct dentry *dentry, unsigned int dir_flags,
		      struct dentry *parent)
{
	int err;

	di_write_lock_child(dentry);
	di_read_lock_parent(parent, AuLock_IR);
	err = au_refresh_dentry(dentry, parent);
	di_read_unlock(parent, AuLock_IR);
	di_write_unlock(dentry);

	return err;
}

static int au_do_refresh_d(struct dentry *dentry, unsigned int sigen,
			   struct au_sbinfo *sbinfo,
			   const unsigned int dir_flags)
{
	int err;
	struct dentry *parent;
	struct inode *inode;

	err = 0;
	parent = dget_parent(dentry);
	if (!au_digen_test(parent, sigen) && au_digen_test(dentry, sigen)) {
		inode = dentry->d_inode;
		if (inode) {
			if (!S_ISDIR(inode->i_mode))
				err = au_do_refresh(dentry, /*dir_flags*/0,
						 parent);
			else {
				err = au_do_refresh(dentry, dir_flags, parent);
				if (unlikely(err))
					au_fset_si(sbinfo, FAILED_REFRESH_DIR);
			}
		} else
			err = au_do_refresh(dentry, /*dir_flags*/0, parent);
		AuDbgDentry(dentry);
	}
	dput(parent);

	AuTraceErr(err);
	return err;
}

static int au_refresh_d(struct super_block *sb)
{
	int err, i, j, ndentry, e;
	unsigned int sigen;
	struct au_dcsub_pages dpages;
	struct au_dpage *dpage;
	struct dentry **dentries, *d;
	struct au_sbinfo *sbinfo;
	struct dentry *root = sb->s_root;
	const unsigned int dir_flags = au_hi_flags(root->d_inode, /*isdir*/1);

	err = au_dpages_init(&dpages, GFP_NOFS);
	if (unlikely(err))
		goto out;
	err = au_dcsub_pages(&dpages, root, NULL, NULL);
	if (unlikely(err))
		goto out_dpages;

	sigen = au_sigen(sb);
	sbinfo = au_sbi(sb);
	for (i = 0; i < dpages.ndpage; i++) {
		dpage = dpages.dpages + i;
		dentries = dpage->dentries;
		ndentry = dpage->ndentry;
		for (j = 0; j < ndentry; j++) {
			d = dentries[j];
			e = au_do_refresh_d(d, sigen, sbinfo, dir_flags);
			if (unlikely(e && !err))
				err = e;
			/* go on even err */
		}
	}

out_dpages:
	au_dpages_free(&dpages);
out:
	return err;
}

static int au_refresh_i(struct super_block *sb)
{
	int err, e;
	unsigned int sigen;
	unsigned long long max, ull;
	struct inode *inode, **array;

	array = au_iarray_alloc(sb, &max);
	err = PTR_ERR(array);
	if (IS_ERR(array))
		goto out;

	err = 0;
	sigen = au_sigen(sb);
	for (ull = 0; ull < max; ull++) {
		inode = array[ull];
		if (unlikely(!inode))
			break;
		if (au_iigen(inode, NULL) != sigen) {
			ii_write_lock_child(inode);
			e = au_refresh_hinode_self(inode);
			ii_write_unlock(inode);
			if (unlikely(e)) {
				pr_err("error %d, i%lu\n", e, inode->i_ino);
				if (!err)
					err = e;
				/* go on even if err */
			}
		}
	}

	au_iarray_free(array, max);

out:
	return err;
}

static void au_remount_refresh(struct super_block *sb)
{
	int err, e;
	unsigned int udba;
	aufs_bindex_t bend;
	struct dentry *root;
	struct inode *inode;

	au_sigen_inc(sb);
	au_fclr_si(au_sbi(sb), FAILED_REFRESH_DIR);

	root = sb->s_root;
	DiMustNoWaiters(root);
	inode = root->d_inode;
	IiMustNoWaiters(inode);

	udba = au_opt_udba(sb);
	bend = au_sbend(sb);
	di_write_unlock(root);
	err = au_refresh_d(sb);
	e = au_refresh_i(sb);
	if (unlikely(e && !err))
		err = e;
	/* aufs_write_lock() calls ..._child() */
	di_write_lock_child(root);

	au_cpup_attr_all(inode, /*force*/1);

	if (unlikely(err))
		AuIOErr("refresh failed, ignored, %d\n", err);
}

/* stop extra interpretation of errno in mount(8), and strange error messages */
static int cvt_err(int err)
{
	AuTraceErr(err);

	switch (err) {
	case -ENOENT:
	case -ENOTDIR:
	case -EEXIST:
	case -EIO:
		err = -EINVAL;
	}
	return err;
}

static int aufs_remount_fs(struct super_block *sb, int *flags, char *data)
{
	int err, do_dx;
	unsigned int mntflags;
	struct au_opts opts;
	struct dentry *root;
	struct inode *inode;
	struct au_sbinfo *sbinfo;

	err = 0;
	root = sb->s_root;
	if (!data || !*data) {
		err = si_write_lock(sb, AuLock_FLUSH | AuLock_NOPLM);
		if (!err) {
			di_write_lock_child(root);
			err = au_opts_verify(sb, *flags, /*pending*/0);
			aufs_write_unlock(root);
		}
		goto out;
	}

	err = -ENOMEM;
	memset(&opts, 0, sizeof(opts));
	opts.opt = (void *)__get_free_page(GFP_NOFS);
	if (unlikely(!opts.opt))
		goto out;
	opts.max_opt = PAGE_SIZE / sizeof(*opts.opt);
	opts.flags = AuOpts_REMOUNT;
	opts.sb_flags = *flags;

	/* parse it before aufs lock */
	err = au_opts_parse(sb, data, &opts);
	if (unlikely(err))
		goto out_opts;

	sbinfo = au_sbi(sb);
	inode = root->d_inode;
	mutex_lock(&inode->i_mutex);
	err = si_write_lock(sb, AuLock_FLUSH | AuLock_NOPLM);
	if (unlikely(err))
		goto out_mtx;
	di_write_lock_child(root);

	/* au_opts_remount() may return an error */
	err = au_opts_remount(sb, &opts);
	au_opts_free(&opts);

	if (au_ftest_opts(opts.flags, REFRESH))
		au_remount_refresh(sb);

	if (au_ftest_opts(opts.flags, REFRESH_DYAOP)) {
		mntflags = au_mntflags(sb);
		do_dx = !!au_opt_test(mntflags, DIO);
		au_dy_arefresh(do_dx);
	}

	aufs_write_unlock(root);

out_mtx:
	mutex_unlock(&inode->i_mutex);
out_opts:
	free_page((unsigned long)opts.opt);
out:
	err = cvt_err(err);
	AuTraceErr(err);
	return err;
}

static const struct super_operations aufs_sop = {
	.alloc_inode	= aufs_alloc_inode,
	.destroy_inode	= aufs_destroy_inode,
	/* always deleting, no clearing */
	.drop_inode	= generic_delete_inode,
	.put_super	= aufs_put_super,
	.remount_fs	= aufs_remount_fs
};

/* ---------------------------------------------------------------------- */

static int alloc_root(struct super_block *sb)
{
	int err;
	struct inode *inode;
	struct dentry *root;

	err = -ENOMEM;
	inode = au_iget_locked(sb, AUFS_ROOT_INO);
	err = PTR_ERR(inode);
	if (IS_ERR(inode))
		goto out;

	inode->i_op = &aufs_dir_iop;
	inode->i_fop = &simple_dir_operations; /* replace later */
	inode->i_mode = S_IFDIR;
	set_nlink(inode, 2);
	unlock_new_inode(inode);

	root = d_make_root(inode);
	if (unlikely(!root))
		goto out;
	err = PTR_ERR(root);
	if (IS_ERR(root))
		goto out;

	err = au_di_init(root);
	if (!err) {
		sb->s_root = root;
		return 0; /* success */
	}
	dput(root);

out:
	return err;
}

static int aufs_fill_super(struct super_block *sb, void *raw_data,
			   int silent __maybe_unused)
{
	int err;
	struct au_opts opts;
	struct dentry *root;
	struct inode *inode;
	char *arg = raw_data;

	if (unlikely(!arg || !*arg)) {
		err = -EINVAL;
		pr_err("no arg\n");
		goto out;
	}

	err = -ENOMEM;
	memset(&opts, 0, sizeof(opts));
	opts.opt = (void *)__get_free_page(GFP_NOFS);
	if (unlikely(!opts.opt))
		goto out;
	opts.max_opt = PAGE_SIZE / sizeof(*opts.opt);
	opts.sb_flags = sb->s_flags;

	err = au_si_alloc(sb);
	if (unlikely(err))
		goto out_opts;

	/* all timestamps always follow the ones on the branch */
	sb->s_flags |= MS_NOATIME | MS_NODIRATIME;
	sb->s_op = &aufs_sop;
	sb->s_d_op = &aufs_dop;
	sb->s_magic = AUFS_SUPER_MAGIC;
	sb->s_maxbytes = 0;
	sb->s_stack_depth = 1;

	err = alloc_root(sb);
	if (unlikely(err)) {
		si_write_unlock(sb);
		goto out_info;
	}
	root = sb->s_root;
	inode = root->d_inode;

	/*
	 * actually we can parse options regardless aufs lock here.
	 * but at remount time, parsing must be done before aufs lock.
	 * so we follow the same rule.
	 */
	ii_write_lock_parent(inode);
	aufs_write_unlock(root);
	err = au_opts_parse(sb, arg, &opts);
	if (unlikely(err))
		goto out_root;

	/* lock vfs_inode first, then aufs. */
	mutex_lock(&inode->i_mutex);
	aufs_write_lock(root);
	err = au_opts_mount(sb, &opts);
	au_opts_free(&opts);
	aufs_write_unlock(root);
	mutex_unlock(&inode->i_mutex);
	if (!err)
		goto out_opts; /* success */

out_root:
	dput(root);
	sb->s_root = NULL;
out_info:
	kobject_put(&au_sbi(sb)->si_kobj);
	sb->s_fs_info = NULL;
out_opts:
	free_page((unsigned long)opts.opt);
out:
	AuTraceErr(err);
	err = cvt_err(err);
	AuTraceErr(err);
	return err;
}

/* ---------------------------------------------------------------------- */

static struct dentry *aufs_mount(struct file_system_type *fs_type, int flags,
				 const char *dev_name __maybe_unused,
				 void *raw_data)
{
	struct dentry *root;
	struct super_block *sb;

	/* all timestamps always follow the ones on the branch */
	/* mnt->mnt_flags |= MNT_NOATIME | MNT_NODIRATIME; */
	root = mount_nodev(fs_type, flags, raw_data, aufs_fill_super);
	if (IS_ERR(root))
		goto out;

	sb = root->d_sb;
	si_write_lock(sb, !AuLock_FLUSH);
	sysaufs_brs_add(sb, 0);
	si_write_unlock(sb);
	au_sbilist_add(sb);

out:
	return root;
}

static void aufs_kill_sb(struct super_block *sb)
{
	struct au_sbinfo *sbinfo;

	sbinfo = au_sbi(sb);
	if (sbinfo) {
		au_sbilist_del(sb);
		aufs_write_lock(sb->s_root);
		if (sbinfo->si_wbr_create_ops->fin)
			sbinfo->si_wbr_create_ops->fin(sb);
		if (au_opt_test(sbinfo->si_mntflags, PLINK))
			au_plink_put(sb, /*verbose*/1);
		au_xino_clr(sb);
		sbinfo->si_sb = NULL;
		aufs_write_unlock(sb->s_root);
		au_nwt_flush(&sbinfo->si_nowait);
	}
	kill_anon_super(sb);
}

struct file_system_type aufs_fs_type = {
	.name		= AUFS_FSTYPE,
	/* a race between rename and others */
	.fs_flags	= FS_RENAME_DOES_D_MOVE,
	.mount		= aufs_mount,
	.kill_sb	= aufs_kill_sb,
	/* no need to __module_get() and module_put(). */
	.owner		= THIS_MODULE,
};
