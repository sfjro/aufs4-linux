// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2005-2020 Junjiro R. Okajima
 */

/*
 * sysfs interface and lifetime management
 * they are necessary regardless sysfs is disabled.
 */

#include <linux/random.h>
#include "aufs.h"

unsigned long sysaufs_si_mask;
struct kset *sysaufs_kset;

#define AuSiAttr(_name) { \
	.attr   = { .name = __stringify(_name), .mode = 0444 },	\
	.show   = sysaufs_si_##_name,				\
}

static struct sysaufs_si_attr sysaufs_si_attr_xi_path = AuSiAttr(xi_path);
struct attribute *sysaufs_si_attrs[] = {
	&sysaufs_si_attr_xi_path.attr,
	NULL,
};

static const struct sysfs_ops au_sbi_ops = {
	.show   = sysaufs_si_show
};

static struct kobj_type au_sbi_ktype = {
	.release	= au_si_free,
	.sysfs_ops	= &au_sbi_ops,
	.default_attrs	= sysaufs_si_attrs
};

/* ---------------------------------------------------------------------- */

int sysaufs_si_init(struct au_sbinfo *sbinfo)
{
	int err;

	sbinfo->si_kobj.kset = sysaufs_kset;
	/* cf. sysaufs_name() */
	err = kobject_init_and_add
		(&sbinfo->si_kobj, &au_sbi_ktype, /*&sysaufs_kset->kobj*/NULL,
		 SysaufsSiNamePrefix "%lx", sysaufs_si_id(sbinfo));

	return err;
}

void sysaufs_fin(void)
{
	sysfs_remove_group(&sysaufs_kset->kobj, sysaufs_attr_group);
	kset_unregister(sysaufs_kset);
}

int __init sysaufs_init(void)
{
	int err;

	do {
		get_random_bytes(&sysaufs_si_mask, sizeof(sysaufs_si_mask));
	} while (!sysaufs_si_mask);

	err = -EINVAL;
	sysaufs_kset = kset_create_and_add(AUFS_NAME, NULL, fs_kobj);
	if (unlikely(!sysaufs_kset))
		goto out;
	err = PTR_ERR(sysaufs_kset);
	if (IS_ERR(sysaufs_kset))
		goto out;
	err = sysfs_create_group(&sysaufs_kset->kobj, sysaufs_attr_group);
	if (unlikely(err))
		kset_unregister(sysaufs_kset);

out:
	return err;
}
