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
 * lookup and dentry operations
 */

#include "aufs.h"

struct dentry *au_sio_lkup_one(struct qstr *name, struct dentry *parent)
{
	struct dentry *dentry;
	int wkq_err;

	if (!au_test_h_perm_sio(parent->d_inode, MAY_EXEC))
		dentry = vfsub_lkup_one(name, parent);
	else {
		struct vfsub_lkup_one_args args = {
			.errp	= &dentry,
			.name	= name,
			.parent	= parent
		};

		wkq_err = au_wkq_wait(vfsub_call_lkup_one, &args);
		if (unlikely(wkq_err))
			dentry = ERR_PTR(wkq_err);
	}

	return dentry;
}

/*
 * lookup @dentry on @bindex which should be negative.
 */
int au_lkup_neg(struct dentry *dentry, aufs_bindex_t bindex, int wh)
{
	int err;
	struct dentry *parent, *h_parent, *h_dentry;
	struct au_branch *br;

	parent = dget_parent(dentry);
	h_parent = au_h_dptr(parent, bindex);
	br = au_sbr(dentry->d_sb, bindex);
	if (wh)
		h_dentry = au_whtmp_lkup(h_parent, br, &dentry->d_name);
	else
		h_dentry = au_sio_lkup_one(&dentry->d_name, h_parent);
	err = PTR_ERR(h_dentry);
	if (IS_ERR(h_dentry))
		goto out;
	if (unlikely(h_dentry->d_inode)) {
		err = -EIO;
		AuIOErr("%pd should be negative on b%d.\n", h_dentry, bindex);
		dput(h_dentry);
		goto out;
	}

	err = 0;
	if (bindex < au_dbstart(dentry))
		au_set_dbstart(dentry, bindex);
	if (au_dbend(dentry) < bindex)
		au_set_dbend(dentry, bindex);
	au_set_h_dptr(dentry, bindex, h_dentry);

out:
	dput(parent);
	return err;
}

/* ---------------------------------------------------------------------- */

/* subset of struct inode */
struct au_iattr {
	unsigned long		i_ino;
	/* unsigned int		i_nlink; */
	kuid_t			i_uid;
	kgid_t			i_gid;
	u64			i_version;
/*
	loff_t			i_size;
	blkcnt_t		i_blocks;
*/
	umode_t			i_mode;
};

static void au_iattr_save(struct au_iattr *ia, struct inode *h_inode)
{
	ia->i_ino = h_inode->i_ino;
	/* ia->i_nlink = h_inode->i_nlink; */
	ia->i_uid = h_inode->i_uid;
	ia->i_gid = h_inode->i_gid;
	ia->i_version = h_inode->i_version;
/*
	ia->i_size = h_inode->i_size;
	ia->i_blocks = h_inode->i_blocks;
*/
	ia->i_mode = (h_inode->i_mode & S_IFMT);
}

static int au_iattr_test(struct au_iattr *ia, struct inode *h_inode)
{
	return ia->i_ino != h_inode->i_ino
		/* || ia->i_nlink != h_inode->i_nlink */
		|| !uid_eq(ia->i_uid, h_inode->i_uid)
		|| !gid_eq(ia->i_gid, h_inode->i_gid)
		|| ia->i_version != h_inode->i_version
/*
		|| ia->i_size != h_inode->i_size
		|| ia->i_blocks != h_inode->i_blocks
*/
		|| ia->i_mode != (h_inode->i_mode & S_IFMT);
}

static int au_h_verify_dentry(struct dentry *h_dentry, struct dentry *h_parent,
			      struct au_branch *br)
{
	int err;
	struct au_iattr ia;
	struct inode *h_inode;
	struct dentry *h_d;
	struct super_block *h_sb;

	err = 0;
	memset(&ia, -1, sizeof(ia));
	h_sb = h_dentry->d_sb;
	h_inode = h_dentry->d_inode;
	if (h_inode)
		au_iattr_save(&ia, h_inode);
	else if (au_test_nfs(h_sb))
		/* nfs d_revalidate may return 0 for negative dentry */
		goto out;

	/* main purpose is namei.c:cached_lookup() and d_revalidate */
	h_d = vfsub_lkup_one(&h_dentry->d_name, h_parent);
	err = PTR_ERR(h_d);
	if (IS_ERR(h_d))
		goto out;

	err = 0;
	if (unlikely(h_d != h_dentry
		     || h_d->d_inode != h_inode
		     || (h_inode && au_iattr_test(&ia, h_inode))))
		err = -EBUSY;
	dput(h_d);

out:
	AuTraceErr(err);
	return err;
}

int au_h_verify(struct dentry *h_dentry, unsigned int udba, struct inode *h_dir,
		struct dentry *h_parent, struct au_branch *br)
{
	int err;

	err = 0;
	if (udba == AuOpt_UDBA_REVAL
	    && !au_test_fs_remote(h_dentry->d_sb)) {
		IMustLock(h_dir);
		err = (h_dentry->d_parent->d_inode != h_dir);
	} else if (udba != AuOpt_UDBA_NONE)
		err = au_h_verify_dentry(h_dentry, h_parent, br);

	return err;
}
