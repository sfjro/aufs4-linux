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
 * module global variables and operations
 */

#include <linux/module.h>
#include <linux/seq_file.h>
#include "aufs.h"

void *au_kzrealloc(void *p, unsigned int nused, unsigned int new_sz, gfp_t gfp)
{
	if (new_sz <= nused)
		return p;

	p = krealloc(p, new_sz, gfp);
	if (p)
		memset(p + nused, 0, new_sz - nused);
	return p;
}

/* ---------------------------------------------------------------------- */

/*
 * aufs caches
 */
struct kmem_cache *au_cachep[AuCache_Last];
static int __init au_cache_init(void)
{
	au_cachep[AuCache_DINFO] = AuCacheCtor(au_dinfo, au_di_init_once);
	if (au_cachep[AuCache_DINFO])
		/* SLAB_DESTROY_BY_RCU */
		au_cachep[AuCache_ICNTNR] = AuCacheCtor(au_icntnr,
							au_icntnr_init_once);
	if (au_cachep[AuCache_ICNTNR])
		return 0;

	return -ENOMEM;
}

static void au_cache_fin(void)
{
	int i;

	/*
	 * Make sure all delayed rcu free inodes are flushed before we
	 * destroy cache.
	 */
	rcu_barrier();
	for (i = 0; i < AuCache_Last; i++)
		if (au_cachep[i]) {
			kmem_cache_destroy(au_cachep[i]);
			au_cachep[i] = NULL;
		}
}

/* ---------------------------------------------------------------------- */

#ifdef CONFIG_AUFS_SBILIST
/*
 * iterate_supers_type() doesn't protect us from
 * remounting (branch management)
 */
struct au_splhead au_sbilist;
#endif

/*
 * functions for module interface.
 */
MODULE_LICENSE("GPL");
/* MODULE_LICENSE("GPL v2"); */
MODULE_AUTHOR("Junjiro R. Okajima <aufs-users@lists.sourceforge.net>");
MODULE_DESCRIPTION(AUFS_NAME
	" -- Advanced multi layered unification filesystem");
MODULE_VERSION(AUFS_VERSION);

/* this module parameter has no meaning when SYSFS is disabled */
int sysaufs_brs = 1;
MODULE_PARM_DESC(brs, "use <sysfs>/fs/aufs/si_*/brN");
module_param_named(brs, sysaufs_brs, int, S_IRUGO);

/* ---------------------------------------------------------------------- */

static char au_esc_chars[0x20 + 3]; /* 0x01-0x20, backslash, del, and NULL */

int au_seq_path(struct seq_file *seq, struct path *path)
{
	return seq_path(seq, path, au_esc_chars);
}

/* ---------------------------------------------------------------------- */

static int __init aufs_init(void)
{
	int err, i;
	char *p;

	p = au_esc_chars;
	for (i = 1; i <= ' '; i++)
		*p++ = i;
	*p++ = '\\';
	*p++ = '\x7f';
	*p = 0;

	au_sbilist_init();
	sysaufs_brs_init();
	err = sysaufs_init();
	if (unlikely(err))
		goto out;
	err = au_wkq_init();
	if (unlikely(err))
		goto out_sysaufs;
	err = au_cache_init();
	if (unlikely(err))
		goto out_wkq;

	/* since we define pr_fmt, call printk directly */
	printk(KERN_INFO AUFS_NAME " " AUFS_VERSION "\n");
	goto out; /* success */

	au_cache_fin();
out_wkq:
	au_wkq_fin();
out_sysaufs:
	sysaufs_fin();
out:
	return err;
}

static void __exit aufs_exit(void)
{
	au_cache_fin();
	au_wkq_fin();
	sysaufs_fin();
}

module_init(aufs_init);
module_exit(aufs_exit);
