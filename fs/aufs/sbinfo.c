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
 * superblock private data
 */

#include "aufs.h"

/*
 * they are necessary regardless sysfs is disabled.
 */
void au_si_free(struct kobject *kobj)
{
	struct au_sbinfo *sbinfo;

	sbinfo = container_of(kobj, struct au_sbinfo, si_kobj);
	kfree(sbinfo->si_branch);
	AuRwDestroy(&sbinfo->si_rwsem);

	kfree(sbinfo);
}

int au_si_alloc(struct super_block *sb)
{
	int err;
	struct au_sbinfo *sbinfo;
	static struct lock_class_key aufs_si;

	err = -ENOMEM;
	sbinfo = kzalloc(sizeof(*sbinfo), GFP_NOFS);
	if (unlikely(!sbinfo))
		goto out;

	/* will be reallocated separately */
	sbinfo->si_branch = kzalloc(sizeof(*sbinfo->si_branch), GFP_NOFS);
	if (unlikely(!sbinfo->si_branch))
		goto out_sbinfo;

	au_rw_init_wlock(&sbinfo->si_rwsem);
	au_rw_class(&sbinfo->si_rwsem, &aufs_si);

	sbinfo->si_bend = -1;

	/* leave other members for sysaufs and si_mnt. */
	sb->s_fs_info = sbinfo;
	return 0; /* success */

	kfree(sbinfo->si_branch);
out_sbinfo:
	kfree(sbinfo);
out:
	return err;
}
