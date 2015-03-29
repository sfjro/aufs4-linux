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
 * virtual or vertical directory
 */

#include "aufs.h"

void au_vdir_free(struct au_vdir *vdir)
{
	unsigned char **deblk;

	deblk = vdir->vd_deblk;
	while (vdir->vd_nblk--)
		kfree(*deblk++);
	kfree(vdir->vd_deblk);
	au_cache_free_vdir(vdir);
}
