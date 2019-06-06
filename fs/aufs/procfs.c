// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2010-2019 Junjiro R. Okajima
 */

/*
 * procfs interfaces
 */

#include <linux/proc_fs.h>
#include "aufs.h"

static int au_procfs_plm_release(struct inode *inode, struct file *file)
{
	struct au_sbinfo *sbinfo;

	sbinfo = file->private_data;
	if (sbinfo) {
		au_plink_maint_leave(sbinfo);
		kobject_put(&sbinfo->si_kobj);
	}

	return 0;
}

static void au_procfs_plm_write_clean(struct file *file)
{
	struct au_sbinfo *sbinfo;

	sbinfo = file->private_data;
	if (sbinfo)
		au_plink_clean(sbinfo->si_sb, /*verbose*/0);
}

static int au_procfs_plm_write_si(struct file *file, unsigned long id)
{
	int err;
	struct super_block *sb;
	struct au_sbinfo *sbinfo;
	struct hlist_bl_node *pos;

	err = -EBUSY;
	if (unlikely(file->private_data))
		goto out;

	sb = NULL;
	/* don't use au_sbilist_lock() here */
	hlist_bl_lock(&au_sbilist);
	hlist_bl_for_each_entry(sbinfo, pos, &au_sbilist, si_list)
		if (id == sysaufs_si_id(sbinfo)) {
			if (kobject_get_unless_zero(&sbinfo->si_kobj))
				sb = sbinfo->si_sb;
			break;
		}
	hlist_bl_unlock(&au_sbilist);

	err = -EINVAL;
	if (unlikely(!sb))
		goto out;

	err = au_plink_maint_enter(sb);
	if (!err)
		/* keep kobject_get() */
		file->private_data = sbinfo;
	else
		kobject_put(&sbinfo->si_kobj);
out:
	return err;
}

/*
 * Accept a valid "si=xxxx" only.
 * Once it is accepted successfully, accept "clean" too.
 */
static ssize_t au_procfs_plm_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	ssize_t err;
	unsigned long id;
	/* last newline is allowed */
	char buf[3 + sizeof(unsigned long) * 2 + 1];

	err = -EACCES;
	if (unlikely(!capable(CAP_SYS_ADMIN)))
		goto out;

	err = -EINVAL;
	if (unlikely(count > sizeof(buf)))
		goto out;

	err = copy_from_user(buf, ubuf, count);
	if (unlikely(err)) {
		err = -EFAULT;
		goto out;
	}
	buf[count] = 0;

	err = -EINVAL;
	if (!strcmp("clean", buf)) {
		au_procfs_plm_write_clean(file);
		goto out_success;
	} else if (unlikely(strncmp("si=", buf, 3)))
		goto out;

	err = kstrtoul(buf + 3, 16, &id);
	if (unlikely(err))
		goto out;

	err = au_procfs_plm_write_si(file, id);
	if (unlikely(err))
		goto out;

out_success:
	err = count; /* success */
out:
	return err;
}

static const struct file_operations au_procfs_plm_fop = {
	.write		= au_procfs_plm_write,
	.release	= au_procfs_plm_release,
	.owner		= THIS_MODULE
};

/* ---------------------------------------------------------------------- */

static struct proc_dir_entry *au_procfs_dir;

void au_procfs_fin(void)
{
	remove_proc_entry(AUFS_PLINK_MAINT_NAME, au_procfs_dir);
	remove_proc_entry(AUFS_PLINK_MAINT_DIR, NULL);
}

int __init au_procfs_init(void)
{
	int err;
	struct proc_dir_entry *entry;

	err = -ENOMEM;
	au_procfs_dir = proc_mkdir(AUFS_PLINK_MAINT_DIR, NULL);
	if (unlikely(!au_procfs_dir))
		goto out;

	entry = proc_create(AUFS_PLINK_MAINT_NAME, S_IFREG | 0200,
			    au_procfs_dir, &au_procfs_plm_fop);
	if (unlikely(!entry))
		goto out_dir;

	err = 0;
	goto out; /* success */


out_dir:
	remove_proc_entry(AUFS_PLINK_MAINT_DIR, NULL);
out:
	return err;
}
