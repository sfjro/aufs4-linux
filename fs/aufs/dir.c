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
 * directory operations
 */

#include "aufs.h"

void au_add_nlink(struct inode *dir, struct inode *h_dir)
{
	unsigned int nlink;

	AuDebugOn(!S_ISDIR(dir->i_mode) || !S_ISDIR(h_dir->i_mode));

	nlink = dir->i_nlink;
	nlink += h_dir->i_nlink - 2;
	if (h_dir->i_nlink < 2)
		nlink += 2;
	smp_mb(); /* for i_nlink */
	/* 0 can happen in revaliding */
	set_nlink(dir, nlink);
}

void au_sub_nlink(struct inode *dir, struct inode *h_dir)
{
	unsigned int nlink;

	AuDebugOn(!S_ISDIR(dir->i_mode) || !S_ISDIR(h_dir->i_mode));

	nlink = dir->i_nlink;
	nlink -= h_dir->i_nlink - 2;
	if (h_dir->i_nlink < 2)
		nlink -= 2;
	smp_mb(); /* for i_nlink */
	/* nlink == 0 means the branch-fs is broken */
	set_nlink(dir, nlink);
}

loff_t au_dir_size(struct file *file, struct dentry *dentry)
{
	loff_t sz;
	aufs_bindex_t bindex, bend;
	struct file *h_file;
	struct dentry *h_dentry;

	sz = 0;
	if (file) {
		AuDebugOn(!d_is_dir(file->f_path.dentry));

		bend = au_fbend_dir(file);
		for (bindex = au_fbstart(file);
		     bindex <= bend && sz < KMALLOC_MAX_SIZE;
		     bindex++) {
			h_file = au_hf_dir(file, bindex);
			if (h_file && file_inode(h_file))
				sz += vfsub_f_size_read(h_file);
		}
	} else {
		AuDebugOn(!dentry);
		AuDebugOn(!d_is_dir(dentry));

		bend = au_dbtaildir(dentry);
		for (bindex = au_dbstart(dentry);
		     bindex <= bend && sz < KMALLOC_MAX_SIZE;
		     bindex++) {
			h_dentry = au_h_dptr(dentry, bindex);
			if (h_dentry && h_dentry->d_inode)
				sz += i_size_read(h_dentry->d_inode);
		}
	}
	if (sz < KMALLOC_MAX_SIZE)
		sz = roundup_pow_of_two(sz);
	if (sz > KMALLOC_MAX_SIZE)
		sz = KMALLOC_MAX_SIZE;
	else if (sz < NAME_MAX) {
		BUILD_BUG_ON(AUFS_RDBLK_DEF < NAME_MAX);
		sz = AUFS_RDBLK_DEF;
	}
	return sz;
}
