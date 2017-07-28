/*
 * Copyright (C) 2017 Junjiro R. Okajima
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
 * special handling in renaming a directoy
 * in order to support looking-up the before-renamed name on the lower readonly
 * branches
 */

#include <linux/byteorder/generic.h>
#include "aufs.h"

static void au_dr_hino_del(struct au_dr_br *dr, struct au_dr_hino *ent)
{
	int idx;

	idx = au_dr_ihash(ent->dr_h_ino);
	au_hbl_del(&ent->dr_hnode, dr->dr_h_ino + idx);
}

static int au_dr_hino_test_empty(struct au_dr_br *dr)
{
	int ret, i;
	struct hlist_bl_head *hbl;

	ret = 1;
	for (i = 0; ret && i < AuDirren_NHASH; i++) {
		hbl = dr->dr_h_ino + i;
		hlist_bl_lock(hbl);
		ret &= hlist_bl_empty(hbl);
		hlist_bl_unlock(hbl);
	}

	return ret;
}

static struct au_dr_hino *au_dr_hino_find(struct au_dr_br *dr, ino_t ino)
{
	struct au_dr_hino *found, *ent;
	struct hlist_bl_head *hbl;
	struct hlist_bl_node *pos;
	int idx;

	found = NULL;
	idx = au_dr_ihash(ino);
	hbl= dr->dr_h_ino + idx;
	hlist_bl_lock(hbl);
	hlist_bl_for_each_entry(ent, pos, hbl, dr_hnode)
		if (ent->dr_h_ino == ino) {
			found = ent;
			break;
		}
	hlist_bl_unlock(hbl);

	return found;
}

int au_dr_hino_test_add(struct au_dr_br *dr, ino_t ino,
			struct au_dr_hino *add_ent)
{
	int found, idx;
	struct hlist_bl_head *hbl;
	struct hlist_bl_node *pos;
	struct au_dr_hino *ent;

	found = 0;
	idx = au_dr_ihash(ino);
	hbl = dr->dr_h_ino + idx;
#if 0
	{
		struct hlist_bl_node *tmp;
		hlist_bl_for_each_entry_safe(ent, pos, tmp, hbl, dr_hnode)
			AuDbg("hi%llu\n", (unsigned long long)ent->dr_h_ino);
	}
#endif
	hlist_bl_lock(hbl);
	hlist_bl_for_each_entry(ent, pos, hbl, dr_hnode)
		if (ent->dr_h_ino == ino) {
			found = 1;
			break;
		}
	if (!found && add_ent)
		hlist_bl_add_head(&add_ent->dr_hnode, hbl);
	hlist_bl_unlock(hbl);

	if (!found && add_ent)
		AuDbg("i%llu added\n", (unsigned long long)add_ent->dr_h_ino);

	return found;
}

void au_dr_hino_free(struct au_dr_br *dr)
{
	int i;
	struct hlist_bl_head *hbl;
	struct hlist_bl_node *pos, *tmp;
	struct au_dr_hino *ent;

	/* SiMustWriteLock(sb); */

	for (i = 0; i < AuDirren_NHASH; i++) {
		hbl = dr->dr_h_ino + i;
		/* no spinlock since sbinfo must be write-locked */
		hlist_bl_for_each_entry_safe(ent, pos, tmp, hbl, dr_hnode)
			kfree(ent);
		INIT_HLIST_BL_HEAD(hbl);
	}
}

/* returns the number of inodes or an error */
static int au_dr_hino_store(struct super_block *sb, struct au_branch *br,
			    struct file *hinofile)
{
	int err, i;
	ssize_t ssz;
	loff_t pos, oldsize;
	uint64_t u64;
	struct inode *hinoinode;
	struct hlist_bl_head *hbl;
	struct hlist_bl_node *n1, *n2;
	struct au_dr_hino *ent;

	SiMustWriteLock(sb);
	AuDebugOn(!au_br_writable(br->br_perm));

	hinoinode = file_inode(hinofile);
	oldsize = i_size_read(hinoinode);

	err = 0;
	pos = 0;
	hbl = br->br_dirren.dr_h_ino;
	for (i = 0; !err && i < AuDirren_NHASH; i++, hbl++) {
		/* no bit-lock since sbinfo must be write-locked */
		hlist_bl_for_each_entry_safe(ent, n1, n2, hbl, dr_hnode) {
			AuDbg("hi%llu, %pD2\n",
			      (unsigned long long)ent->dr_h_ino, hinofile);
			u64 = cpu_to_be64(ent->dr_h_ino);
			ssz = vfsub_write_k(hinofile, &u64, sizeof(u64), &pos);
			if (ssz == sizeof(u64))
				continue;

			/* write error */
			pr_err("ssz %zd, %pD2\n", ssz, hinofile);
			err = -ENOSPC;
			if (ssz < 0)
				err = ssz;
			break;
		}
	}
	/* regardless the error */
	if (pos < oldsize) {
		err = vfsub_trunc(&hinofile->f_path, pos, /*attr*/0, hinofile);
		AuTraceErr(err);
	}

	AuTraceErr(err);
	return err;
}

static int au_dr_hino_load(struct au_dr_br *dr, struct file *hinofile)
{
	int err, hidx;
	ssize_t ssz;
	size_t sz, n;
	loff_t pos;
	uint64_t u64;
	struct au_dr_hino *ent;
	struct inode *hinoinode;
	struct hlist_bl_head *hbl;

	err = 0;
	pos = 0;
	hbl = dr->dr_h_ino;
	hinoinode = file_inode(hinofile);
	sz = i_size_read(hinoinode);
	AuDebugOn(sz % sizeof(u64));
	n = sz / sizeof(u64);
	while (n--) {
		ssz = vfsub_read_k(hinofile, &u64, sizeof(u64), &pos);
		if (unlikely(ssz != sizeof(u64))) {
			pr_err("ssz %zd, %pD2\n", ssz, hinofile);
			err = -EINVAL;
			if (ssz < 0)
				err = ssz;
			goto out_free;
		}

		ent = kmalloc(sizeof(*ent), GFP_NOFS);
		if (!ent) {
			err = -ENOMEM;
			AuTraceErr(err);
			goto out_free;
		}
		ent->dr_h_ino = be64_to_cpu(u64);
		AuDbg("hi%llu, %pD2\n",
		      (unsigned long long)ent->dr_h_ino, hinofile);
		hidx = au_dr_ihash(ent->dr_h_ino);
		au_hbl_add(&ent->dr_hnode, hbl + hidx);
	}
	goto out; /* success */

out_free:
	au_dr_hino_free(dr);
out:
	AuTraceErr(err);
	return err;
}

/*
 * @bindex/@br is a switch to distinguish whether suspending hnotify or not.
 * @path is a switch to distinguish load and store.
 */
static int au_dr_hino(struct super_block *sb, aufs_bindex_t bindex,
		      struct au_branch *br, const struct path *path)
{
	int err, flags;
	unsigned char load, suspend;
	struct file *hinofile;
	struct au_hinode *hdir;
	struct inode *dir, *delegated;
	struct path hinopath;
	struct qstr hinoname = QSTR_INIT(AUFS_WH_DR_BRHINO,
					 sizeof(AUFS_WH_DR_BRHINO) - 1);

	AuDebugOn(bindex < 0 && !br);
	AuDebugOn(bindex >= 0 && br);

	err = -EINVAL;
	suspend = !br;
	if (suspend)
		br = au_sbr(sb, bindex);
	load = !!path;
	if (!load) {
		path = &br->br_path;
		AuDebugOn(!au_br_writable(br->br_perm));
		if (unlikely(!au_br_writable(br->br_perm)))
			goto out;
	}

	hdir = NULL;
	if (suspend) {
		dir = d_inode(sb->s_root);
		hdir = au_hinode(au_ii(dir), bindex);
		dir = hdir->hi_inode;
		au_hn_inode_lock_nested(hdir, AuLsc_I_CHILD);
	} else {
		dir = d_inode(path->dentry);
		inode_lock_nested(dir, AuLsc_I_CHILD);
	}
	hinopath.dentry = vfsub_lkup_one(&hinoname, path->dentry);
	err = PTR_ERR(hinopath.dentry);
	if (IS_ERR(hinopath.dentry))
		goto out_unlock;

	err = 0;
	flags = O_RDONLY;
	if (load) {
		if (d_is_negative(hinopath.dentry))
			goto out_dput; /* success */
	} else {
		if (au_dr_hino_test_empty(&br->br_dirren)) {
			if (d_is_positive(hinopath.dentry)) {
				delegated = NULL;
				err = vfsub_unlink(dir, &hinopath, &delegated,
						   /*force*/0);
				AuTraceErr(err);
				if (unlikely(err))
					pr_err("ignored err %d, %pd2\n",
					       err, hinopath.dentry);
				if (unlikely(err == -EWOULDBLOCK))
					iput(delegated);
				err = 0;
			}
			goto out_dput;
		} else if (!d_is_positive(hinopath.dentry)) {
			err = vfsub_create(dir, &hinopath, 0600,
					   /*want_excl*/false);
			AuTraceErr(err);
			if (unlikely(err))
				goto out_dput;
		}
		flags = O_WRONLY;
	}
	hinopath.mnt = path->mnt;
	hinofile = vfsub_dentry_open(&hinopath, flags);
	if (suspend)
		au_hn_inode_unlock(hdir);
	else
		inode_unlock(dir);
	dput(hinopath.dentry);
	AuTraceErrPtr(hinofile);
	if (IS_ERR(hinofile)) {
		err = PTR_ERR(hinofile);
		goto out;
	}

	if (load)
		err = au_dr_hino_load(&br->br_dirren, hinofile);
	else
		err = au_dr_hino_store(sb, br, hinofile);
	fput(hinofile);
	goto out;

out_dput:
	dput(hinopath.dentry);
out_unlock:
	if (suspend)
		au_hn_inode_unlock(hdir);
	else
		inode_unlock(dir);
out:
	AuTraceErr(err);
	return err;
}

/* ---------------------------------------------------------------------- */

static int au_dr_brid_init(struct au_dr_brid *brid, const struct path *path)
{
	int err;
	struct kstatfs kstfs;
	dev_t dev;
	struct dentry *dentry;
	struct super_block *sb;

	err = vfs_statfs((void *)path, &kstfs);
	AuTraceErr(err);
	if (unlikely(err))
		goto out;

	/* todo: support for UUID */

	if (kstfs.f_fsid.val[0] || kstfs.f_fsid.val[1]) {
		brid->type = AuBrid_FSID;
		brid->fsid = kstfs.f_fsid;
	} else {
		dentry = path->dentry;
		sb = dentry->d_sb;
		dev = sb->s_dev;
		if (dev) {
			brid->type = AuBrid_DEV;
			brid->dev = dev;
		}
	}

out:
	return err;
}

int au_dr_br_init(struct super_block *sb, struct au_branch *br,
		  const struct path *path)
{
	int err, i;
	struct au_dr_br *dr;
	struct hlist_bl_head *hbl;

	dr = &br->br_dirren;
	hbl = dr->dr_h_ino;
	for (i = 0; i < AuDirren_NHASH; i++, hbl++)
		INIT_HLIST_BL_HEAD(hbl);

	err = au_dr_brid_init(&dr->dr_brid, path);
	if (unlikely(err))
		goto out;

	if (au_opt_test(au_mntflags(sb), DIRREN))
		err = au_dr_hino(sb, /*bindex*/-1, br, path);

out:
	AuTraceErr(err);
	return err;
}

int au_dr_br_fin(struct super_block *sb, struct au_branch *br)
{
	int err;

	err = 0;
	if (au_br_writable(br->br_perm))
		err = au_dr_hino(sb, /*bindex*/-1, br, /*path*/NULL);
	if (!err)
		au_dr_hino_free(&br->br_dirren);

	return err;
}

/* ---------------------------------------------------------------------- */

static int au_brid_str(struct au_dr_brid *brid, struct inode *h_inode,
		       char *buf, size_t sz)
{
	int err;
	unsigned int major, minor;
	char *p;

	p = buf;
	err = snprintf(p, sz, "%d_", brid->type);
	AuDebugOn(err > sz);
	p += err;
	sz -= err;
	switch (brid->type) {
	case AuBrid_Unset:
		return -EINVAL;
	case AuBrid_UUID:
		err = snprintf(p, sz, "%pU", brid->uuid.__u_bits);
		break;
	case AuBrid_FSID:
		err = snprintf(p, sz, "%08x-%08x",
			       brid->fsid.val[0], brid->fsid.val[1]);
		break;
	case AuBrid_DEV:
		major = MAJOR(brid->dev);
		minor = MINOR(brid->dev);
		if (major <= 0xff && minor <= 0xff)
			err = snprintf(p, sz, "%02x%02x", major, minor);
		else
			err = snprintf(p, sz, "%03x:%05x",major, minor);
		break;
	}
	AuDebugOn(err > sz);
	p += err;
	sz -= err;
	err = snprintf(p, sz, "_%llu", (unsigned long long)h_inode->i_ino);
	AuDebugOn(err > sz);
	p += err;
	sz -= err;

	return p - buf;
}
