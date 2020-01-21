/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2005-2020 Junjiro R. Okajima
 */

/*
 * sub-routines for dentry cache
 */

#ifndef __AUFS_DCSUB_H__
#define __AUFS_DCSUB_H__

#ifdef __KERNEL__

#include <linux/dcache.h>
#include <linux/fs.h>

struct au_dpage {
	int ndentry;
	struct dentry **dentries;
};

struct au_dcsub_pages {
	int ndpage;
	struct au_dpage *dpages;
};

/* ---------------------------------------------------------------------- */

/* dcsub.c */
int au_dpages_init(struct au_dcsub_pages *dpages, gfp_t gfp);
void au_dpages_free(struct au_dcsub_pages *dpages);
typedef int (*au_dpages_test)(struct dentry *dentry, void *arg);
int au_dcsub_pages(struct au_dcsub_pages *dpages, struct dentry *root,
		   au_dpages_test test, void *arg);
int au_dcsub_pages_rev(struct au_dcsub_pages *dpages, struct dentry *dentry,
		       int do_include, au_dpages_test test, void *arg);
int au_dcsub_pages_rev_aufs(struct au_dcsub_pages *dpages,
			    struct dentry *dentry, int do_include);
int au_test_subdir(struct dentry *d1, struct dentry *d2);

/* ---------------------------------------------------------------------- */

/*
 * todo: in linux-3.13, several similar (but faster) helpers are added to
 * include/linux/dcache.h. Try them (in the future).
 */

static inline int au_d_hashed_positive(struct dentry *d)
{
	int err;
	struct inode *inode = d_inode(d);

	err = 0;
	if (unlikely(d_unhashed(d)
		     || d_is_negative(d)
		     || !inode->i_nlink))
		err = -ENOENT;
	return err;
}

static inline int au_d_linkable(struct dentry *d)
{
	int err;
	struct inode *inode = d_inode(d);

	err = au_d_hashed_positive(d);
	if (err
	    && d_is_positive(d)
	    && (inode->i_state & I_LINKABLE))
		err = 0;
	return err;
}

static inline int au_d_alive(struct dentry *d)
{
	int err;
	struct inode *inode;

	err = 0;
	if (!IS_ROOT(d))
		err = au_d_hashed_positive(d);
	else {
		inode = d_inode(d);
		if (unlikely(d_unlinked(d)
			     || d_is_negative(d)
			     || !inode->i_nlink))
			err = -ENOENT;
	}
	return err;
}

static inline int au_alive_dir(struct dentry *d)
{
	int err;

	err = au_d_alive(d);
	if (unlikely(err || IS_DEADDIR(d_inode(d))))
		err = -ENOENT;
	return err;
}

static inline int au_qstreq(struct qstr *a, struct qstr *b)
{
	return a->len == b->len
		&& !memcmp(a->name, b->name, a->len);
}

/*
 * by the commit
 * 360f547 2015-01-25 dcache: let the dentry count go down to zero without
 *			taking d_lock
 * the type of d_lockref.count became int, but the inlined function d_count()
 * still returns unsigned int.
 * I don't know why. Maybe it is for every d_count() users?
 * Anyway au_dcount() lives on.
 */
static inline int au_dcount(struct dentry *d)
{
	return (int)d_count(d);
}

#endif /* __KERNEL__ */
#endif /* __AUFS_DCSUB_H__ */
