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

#endif /* __KERNEL__ */
#endif /* __AUFS_FSTYPE_H__ */
