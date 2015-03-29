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
 * file private data
 */

#include "aufs.h"

void au_hfput(struct au_hfile *hf, struct file *file)
{
	/* todo: direct access f_flags */
	if (vfsub_file_flags(file) & __FMODE_EXEC)
		allow_write_access(hf->hf_file);
	fput(hf->hf_file);
	hf->hf_file = NULL;
	atomic_dec(&hf->hf_br->br_count);
	hf->hf_br = NULL;
}

void au_set_h_fptr(struct file *file, aufs_bindex_t bindex, struct file *val)
{
	struct au_finfo *finfo = au_fi(file);
	struct au_hfile *hf;

	AuDebugOn(finfo->fi_btop != bindex);
	hf = &finfo->fi_htop;

	if (hf && hf->hf_file)
		au_hfput(hf, file);
	if (val) {
		FiMustWriteLock(file);
		hf->hf_file = val;
		hf->hf_br = au_sbr(file->f_path.dentry->d_sb, bindex);
	}
}

void au_update_figen(struct file *file)
{
	atomic_set(&au_fi(file)->fi_generation, au_digen(file->f_path.dentry));
	/* smp_mb(); */ /* atomic_set */
}

/* ---------------------------------------------------------------------- */

void au_finfo_fin(struct file *file)
{
	struct au_finfo *finfo;

	finfo = au_fi(file);
	AuRwDestroy(&finfo->fi_rwsem);
	au_cache_free_finfo(finfo);
}

void au_fi_init_once(void *_finfo)
{
	struct au_finfo *finfo = _finfo;
	static struct lock_class_key aufs_fi;

	au_rw_init(&finfo->fi_rwsem);
	au_rw_class(&finfo->fi_rwsem, &aufs_fi);
}

int au_finfo_init(struct file *file)
{
	int err;
	struct au_finfo *finfo;
	struct dentry *dentry;

	err = -ENOMEM;
	dentry = file->f_path.dentry;
	finfo = au_cache_alloc_finfo();
	if (unlikely(!finfo))
		goto out;

	err = 0;
	au_rw_write_lock(&finfo->fi_rwsem);
	finfo->fi_btop = -1;
	atomic_set(&finfo->fi_generation, au_digen(dentry));
	/* smp_mb(); */ /* atomic_set */

	file->private_data = finfo;

out:
	return err;
}
