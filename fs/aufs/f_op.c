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
 * file and vm operations
 */

#include "aufs.h"

int au_do_open_nondir(struct file *file, int flags)
{
	int err;
	aufs_bindex_t bindex;
	struct file *h_file;
	struct dentry *dentry;
	struct au_finfo *finfo;
	struct inode *h_inode;

	FiMustWriteLock(file);

	err = 0;
	dentry = file->f_path.dentry;
	finfo = au_fi(file);
	memset(&finfo->fi_htop, 0, sizeof(finfo->fi_htop));
	bindex = au_dbstart(dentry);
	h_file = au_h_open(dentry, bindex, flags, file);
	if (IS_ERR(h_file))
		err = PTR_ERR(h_file);
	else {
		if ((flags & __O_TMPFILE)
		    && !(flags & O_EXCL)) {
			h_inode = file_inode(h_file);
			spin_lock(&h_inode->i_lock);
			h_inode->i_state |= I_LINKABLE;
			spin_unlock(&h_inode->i_lock);
		}
		au_set_fbstart(file, bindex);
		au_set_h_fptr(file, bindex, h_file);
		au_update_figen(file);
		/* todo: necessary? */
		/* file->f_ra = h_file->f_ra; */
	}

	return err;
}

static int aufs_open_nondir(struct inode *inode __maybe_unused,
			    struct file *file)
{
	int err;
	struct super_block *sb;

	AuDbg("%pD, f_flags 0x%x, f_mode 0x%x\n",
	      file, vfsub_file_flags(file), file->f_mode);

	sb = file->f_path.dentry->d_sb;
	si_read_lock(sb, AuLock_FLUSH);
	err = au_do_open(file, au_do_open_nondir, /*fidir*/NULL);
	si_read_unlock(sb);
	return err;
}

int aufs_release_nondir(struct inode *inode __maybe_unused, struct file *file)
{
	struct au_finfo *finfo;
	aufs_bindex_t bindex;

	finfo = au_fi(file);
	bindex = finfo->fi_btop;
	if (bindex >= 0)
		au_set_h_fptr(file, bindex, NULL);

	au_finfo_fin(file);
	return 0;
}

/* ---------------------------------------------------------------------- */

const struct file_operations aufs_file_fop = {
	.owner		= THIS_MODULE,

	.open		= aufs_open_nondir,
	.release	= aufs_release_nondir
};
