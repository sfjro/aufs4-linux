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
 * mount options/flags
 */

#ifndef __AUFS_OPTS_H__
#define __AUFS_OPTS_H__

#ifdef __KERNEL__

#include <linux/path.h>

struct file;

/* ---------------------------------------------------------------------- */

/* mount flags */
#define AuOpt_XINO		1		/* external inode number bitmap
						   and translation table */

#define AuOpt_Def	AuOpt_XINO

#define au_opt_test(flags, name)	(flags & AuOpt_##name)
#define au_opt_set(flags, name) do { \
	((flags) |= AuOpt_##name); \
} while (0)
#define au_opt_clr(flags, name) do { \
	((flags) &= ~AuOpt_##name); \
} while (0)

/* ---------------------------------------------------------------------- */

struct au_opt_add {
	aufs_bindex_t	bindex;
	char		*pathname;
	int		perm;
	struct path	path;
};

struct au_opt_xino {
	char		*path;
	struct file	*file;
};

/* ---------------------------------------------------------------------- */

void au_optstr_br_perm(au_br_perm_str_t *str, int perm);

#endif /* __KERNEL__ */
#endif /* __AUFS_OPTS_H__ */
