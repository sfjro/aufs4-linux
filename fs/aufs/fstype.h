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
 * judging filesystem type
 */

#ifndef __AUFS_FSTYPE_H__
#define __AUFS_FSTYPE_H__

#ifdef __KERNEL__

#include <linux/fs.h>
#include <linux/magic.h>

static inline int au_test_aufs(struct super_block *sb)
{
	return sb->s_magic == AUFS_SUPER_MAGIC;
}

static inline const char *au_sbtype(struct super_block *sb)
{
	return sb->s_type->name;
}

static inline int au_test_ecryptfs(struct super_block *sb __maybe_unused)
{
#if defined(CONFIG_ECRYPT_FS) || defined(CONFIG_ECRYPT_FS_MODULE)
	return !strcmp(au_sbtype(sb), "ecryptfs");
#else
	return 0;
#endif
}

static inline int au_test_ramfs(struct super_block *sb)
{
	return sb->s_magic == RAMFS_MAGIC;
}

static inline int au_test_procfs(struct super_block *sb __maybe_unused)
{
#ifdef CONFIG_PROC_FS
	return sb->s_magic == PROC_SUPER_MAGIC;
#else
	return 0;
#endif
}

static inline int au_test_sysfs(struct super_block *sb __maybe_unused)
{
#ifdef CONFIG_SYSFS
	return sb->s_magic == SYSFS_MAGIC;
#else
	return 0;
#endif
}

static inline int au_test_configfs(struct super_block *sb __maybe_unused)
{
#if defined(CONFIG_CONFIGFS_FS) || defined(CONFIG_CONFIGFS_FS_MODULE)
	return sb->s_magic == CONFIGFS_MAGIC;
#else
	return 0;
#endif
}

static inline int au_test_securityfs(struct super_block *sb __maybe_unused)
{
#ifdef CONFIG_SECURITYFS
	return sb->s_magic == SECURITYFS_MAGIC;
#else
	return 0;
#endif
}

static inline int au_test_xenfs(struct super_block *sb __maybe_unused)
{
#if defined(CONFIG_XENFS) || defined(CONFIG_XENFS_MODULE)
	return sb->s_magic == XENFS_SUPER_MAGIC;
#else
	return 0;
#endif
}

static inline int au_test_debugfs(struct super_block *sb __maybe_unused)
{
#ifdef CONFIG_DEBUG_FS
	return sb->s_magic == DEBUGFS_MAGIC;
#else
	return 0;
#endif
}

/* ---------------------------------------------------------------------- */
/*
 * they can't be an aufs branch.
 */
static inline int au_test_fs_unsuppoted(struct super_block *sb)
{
	return
		au_test_ramfs(sb) ||
		au_test_procfs(sb)
		|| au_test_sysfs(sb)
		|| au_test_configfs(sb)
		|| au_test_debugfs(sb)
		|| au_test_securityfs(sb)
		|| au_test_xenfs(sb)
		|| au_test_ecryptfs(sb)
		/* || !strcmp(au_sbtype(sb), "unionfs") */
		|| au_test_aufs(sb); /* will be supported in next version */
}

#endif /* __KERNEL__ */
#endif /* __AUFS_FSTYPE_H__ */
