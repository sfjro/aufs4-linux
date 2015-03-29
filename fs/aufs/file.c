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
 * handling file/dir, and address_space operation
 */

#include <linux/fsnotify.h>
#include "aufs.h"

/* drop flags for writing */
unsigned int au_file_roflags(unsigned int flags)
{
	flags &= ~(O_WRONLY | O_RDWR | O_APPEND | O_CREAT | O_TRUNC);
	flags |= O_RDONLY | O_NOATIME;
	return flags;
}

/* common functions to regular file and dir */
struct file *au_h_open(struct dentry *dentry, aufs_bindex_t bindex, int flags,
		       struct file *file)
{
	struct file *h_file;
	struct dentry *h_dentry;
	struct inode *h_inode;
	struct super_block *sb;
	struct au_branch *br;
	struct path h_path;
	int err, exec_flag;

	/* a race condition can happen between open and unlink/rmdir */
	h_file = ERR_PTR(-ENOENT);
	h_dentry = au_h_dptr(dentry, bindex);
	h_inode = h_dentry->d_inode;
	spin_lock(&h_dentry->d_lock);
	err = (!d_unhashed(dentry) && d_unlinked(h_dentry))
		|| !h_inode
		/* || !dentry->d_inode->i_nlink */
		;
	spin_unlock(&h_dentry->d_lock);
	if (unlikely(err))
		goto out;

	sb = dentry->d_sb;
	br = au_sbr(sb, bindex);
	h_file = ERR_PTR(-EACCES);
	exec_flag = flags & __FMODE_EXEC;
	if (exec_flag && (au_br_mnt(br)->mnt_flags & MNT_NOEXEC))
		goto out;

	/* drop flags for writing */
	if (au_test_ro(sb, bindex, dentry->d_inode))
		flags = au_file_roflags(flags);
	flags &= ~O_CREAT;
	atomic_inc(&br->br_count);
	h_path.dentry = h_dentry;
	h_path.mnt = au_br_mnt(br);
	h_file = vfsub_dentry_open(&h_path, flags);
	if (IS_ERR(h_file))
		goto out_br;

	if (exec_flag) {
		err = deny_write_access(h_file);
		if (unlikely(err)) {
			fput(h_file);
			h_file = ERR_PTR(err);
			goto out_br;
		}
	}
	fsnotify_open(h_file);
	goto out; /* success */

out_br:
	atomic_dec(&br->br_count);
out:
	return h_file;
}

/* ---------------------------------------------------------------------- */

static int au_file_refresh_by_inode(struct file *file, int *need_reopen)
{
	int err;
	struct au_pin pin;
	struct au_finfo *finfo;
	struct dentry *parent;
	struct inode *inode;
	struct super_block *sb;
	struct au_cp_generic cpg = {
		.dentry	= file->f_path.dentry,
		.bdst	= -1,
		.bsrc	= -1,
		.len	= -1,
		.pin	= &pin,
		.flags	= AuCpup_DTIME
	};

	FiMustWriteLock(file);

	err = 0;
	finfo = au_fi(file);
	sb = cpg.dentry->d_sb;
	inode = cpg.dentry->d_inode;
	cpg.bdst = au_ibstart(inode);
	if (cpg.bdst == finfo->fi_btop || IS_ROOT(cpg.dentry))
		goto out;

	parent = dget_parent(cpg.dentry);
	if (au_test_ro(sb, cpg.bdst, inode)) {
		di_read_lock_parent(parent, !AuLock_IR);
		err = AuWbrCopyup(au_sbi(sb), cpg.dentry);
		cpg.bdst = err;
		di_read_unlock(parent, !AuLock_IR);
		if (unlikely(err < 0))
			goto out_parent;
		err = 0;
	}

	di_read_lock_parent(parent, AuLock_IR);
	if (!S_ISDIR(inode->i_mode)
	    && au_opt_test(au_mntflags(sb), PLINK)
	    && au_plink_test(inode)
	    && !d_unhashed(cpg.dentry)
	    && cpg.bdst < au_dbstart(cpg.dentry)) {
		err = au_test_and_cpup_dirs(cpg.dentry, cpg.bdst);
		if (unlikely(err))
			goto out_unlock;

		/* always superio. */
		err = au_pin(&pin, cpg.dentry, cpg.bdst, AuOpt_UDBA_NONE,
			     AuPin_DI_LOCKED | AuPin_MNT_WRITE);
		if (!err) {
			err = au_sio_cpup_simple(&cpg);
			au_unpin(&pin);
		}
	}

out_unlock:
	di_read_unlock(parent, AuLock_IR);
out_parent:
	dput(parent);
out:
	return err;
}

static void au_do_refresh_dir(struct file *file)
{
	aufs_bindex_t bindex, bend, new_bindex, brid;
	struct au_hfile *p, tmp, *q;
	struct au_finfo *finfo;
	struct super_block *sb;
	struct au_fidir *fidir;

	FiMustWriteLock(file);

	sb = file->f_path.dentry->d_sb;
	finfo = au_fi(file);
	fidir = finfo->fi_hdir;
	AuDebugOn(!fidir);
	p = fidir->fd_hfile + finfo->fi_btop;
	brid = p->hf_br->br_id;
	bend = fidir->fd_bbot;
	for (bindex = finfo->fi_btop; bindex <= bend; bindex++, p++) {
		if (!p->hf_file)
			continue;

		new_bindex = au_br_index(sb, p->hf_br->br_id);
		if (new_bindex == bindex)
			continue;
		if (new_bindex < 0) {
			au_set_h_fptr(file, bindex, NULL);
			continue;
		}

		/* swap two lower inode, and loop again */
		q = fidir->fd_hfile + new_bindex;
		tmp = *q;
		*q = *p;
		*p = tmp;
		if (tmp.hf_file) {
			bindex--;
			p--;
		}
	}

	p = fidir->fd_hfile;
	if (!d_unlinked(file->f_path.dentry)) {
		bend = au_sbend(sb);
		for (finfo->fi_btop = 0; finfo->fi_btop <= bend;
		     finfo->fi_btop++, p++)
			if (p->hf_file) {
				if (file_inode(p->hf_file))
					break;
				au_hfput(p, file);
			}
	} else {
		bend = au_br_index(sb, brid);
		for (finfo->fi_btop = 0; finfo->fi_btop < bend;
		     finfo->fi_btop++, p++)
			if (p->hf_file)
				au_hfput(p, file);
		bend = au_sbend(sb);
	}

	p = fidir->fd_hfile + bend;
	for (fidir->fd_bbot = bend; fidir->fd_bbot >= finfo->fi_btop;
	     fidir->fd_bbot--, p--)
		if (p->hf_file) {
			if (file_inode(p->hf_file))
				break;
			au_hfput(p, file);
		}
	AuDebugOn(fidir->fd_bbot < finfo->fi_btop);
}

/*
 * after branch manipulating, refresh the file.
 */
static int refresh_file(struct file *file, int (*reopen)(struct file *file))
{
	int err, need_reopen;
	aufs_bindex_t bend, bindex;
	struct dentry *dentry;
	struct au_finfo *finfo;
	struct au_hfile *hfile;

	dentry = file->f_path.dentry;
	finfo = au_fi(file);
	if (!finfo->fi_hdir) {
		hfile = &finfo->fi_htop;
		AuDebugOn(!hfile->hf_file);
		bindex = au_br_index(dentry->d_sb, hfile->hf_br->br_id);
		AuDebugOn(bindex < 0);
		if (bindex != finfo->fi_btop)
			au_set_fbstart(file, bindex);
	} else {
		err = au_fidir_realloc(finfo, au_sbend(dentry->d_sb) + 1);
		if (unlikely(err))
			goto out;
		au_do_refresh_dir(file);
	}

	err = 0;
	need_reopen = 1;
	err = au_file_refresh_by_inode(file, &need_reopen);
	if (!err && need_reopen && !d_unlinked(dentry))
		err = reopen(file);
	if (!err) {
		au_update_figen(file);
		goto out; /* success */
	}

	/* error, close all lower files */
	if (finfo->fi_hdir) {
		bend = au_fbend_dir(file);
		for (bindex = au_fbstart(file); bindex <= bend; bindex++)
			au_set_h_fptr(file, bindex, NULL);
	}

out:
	return err;
}

/* common function to regular file and dir */
int au_reval_and_lock_fdi(struct file *file, int (*reopen)(struct file *file),
			  int wlock)
{
	int err;
	unsigned int sigen, figen;
	aufs_bindex_t bstart;
	unsigned char pseudo_link;
	struct dentry *dentry;
	struct inode *inode;

	err = 0;
	dentry = file->f_path.dentry;
	inode = dentry->d_inode;
	sigen = au_sigen(dentry->d_sb);
	fi_write_lock(file);
	figen = au_figen(file);
	di_write_lock_child(dentry);
	bstart = au_dbstart(dentry);
	pseudo_link = (bstart != au_ibstart(inode));
	if (sigen == figen && !pseudo_link && au_fbstart(file) == bstart) {
		if (!wlock) {
			di_downgrade_lock(dentry, AuLock_IR);
			fi_downgrade_lock(file);
		}
		goto out; /* success */
	}

	AuDbg("sigen %d, figen %d\n", sigen, figen);
	if (au_digen_test(dentry, sigen)) {
		err = au_reval_dpath(dentry, sigen);
		AuDebugOn(!err && au_digen_test(dentry, sigen));
	}

	if (!err)
		err = refresh_file(file, reopen);
	if (!err) {
		if (!wlock) {
			di_downgrade_lock(dentry, AuLock_IR);
			fi_downgrade_lock(file);
		}
	} else {
		di_write_unlock(dentry);
		fi_write_unlock(file);
	}

out:
	return err;
}
