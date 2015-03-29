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
 * directory operations
 */

#ifndef __AUFS_DIR_H__
#define __AUFS_DIR_H__

#ifdef __KERNEL__

#include <linux/fs.h>

/* ---------------------------------------------------------------------- */

/* need to be faster and smaller */

struct au_nhash {
	unsigned int		nh_num;
	struct hlist_head	*nh_head;
};

struct au_vdir_destr {
	unsigned char	len;
	unsigned char	name[0];
} __packed;

struct au_vdir_dehstr {
	struct hlist_node	hash;
	struct au_vdir_destr	*str;
} ____cacheline_aligned_in_smp;

struct au_vdir_de {
	ino_t			de_ino;
	unsigned char		de_type;
	/* caution: packed */
	struct au_vdir_destr	de_str;
} __packed;

struct au_vdir_wh {
	struct hlist_node	wh_hash;
	aufs_bindex_t		wh_bindex;
	/* caution: packed */
	struct au_vdir_destr	wh_str;
} __packed;

union au_vdir_deblk_p {
	unsigned char		*deblk;
	struct au_vdir_de	*de;
};

struct au_vdir {
	unsigned char	**vd_deblk;
	unsigned long	vd_nblk;
	struct {
		unsigned long		ul;
		union au_vdir_deblk_p	p;
	} vd_last;

	unsigned long	vd_version;
	unsigned int	vd_deblk_sz;
	unsigned long	vd_jiffy;
} ____cacheline_aligned_in_smp;

/* ---------------------------------------------------------------------- */

/* dir.c */
loff_t au_dir_size(struct file *file, struct dentry *dentry);

/* vdir.c */
unsigned int au_rdhash_est(loff_t sz);
int au_nhash_alloc(struct au_nhash *nhash, unsigned int num_hash, gfp_t gfp);
void au_nhash_wh_free(struct au_nhash *whlist);
int au_nhash_test_known_wh(struct au_nhash *whlist, char *name, int nlen);
int au_nhash_append_wh(struct au_nhash *whlist, char *name, int nlen, ino_t ino,
		       unsigned int d_type, aufs_bindex_t bindex);
void au_vdir_free(struct au_vdir *vdir);
int au_vdir_init(struct file *file);
int au_vdir_fill_de(struct file *file, struct dir_context *ctx);

#endif /* __KERNEL__ */
#endif /* __AUFS_DIR_H__ */
