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
 * copy-up functions, see wbr_policy.c for copy-down
 */

#include "aufs.h"

void au_cpup_igen(struct inode *inode, struct inode *h_inode)
{
	struct au_iinfo *iinfo = au_ii(inode);

	IiMustWriteLock(inode);

	iinfo->ii_higen = h_inode->i_generation;
	iinfo->ii_hsb1 = h_inode->i_sb;
}
