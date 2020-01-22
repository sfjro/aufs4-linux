/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2005-2020 Junjiro R. Okajima
 */

/*
 * debugfs interface
 */

#ifndef __DBGAUFS_H__
#define __DBGAUFS_H__

#ifdef __KERNEL__

struct super_block;
struct au_sbinfo;
struct au_branch;

#ifdef CONFIG_DEBUG_FS
/* dbgaufs.c */
void dbgaufs_xino_del(struct au_branch *br);
void dbgaufs_brs_del(struct super_block *sb, aufs_bindex_t bindex);
void dbgaufs_brs_add(struct super_block *sb, aufs_bindex_t bindex, int topdown);
void dbgaufs_si_fin(struct au_sbinfo *sbinfo);
int dbgaufs_si_init(struct au_sbinfo *sbinfo);
void dbgaufs_fin(void);
int __init dbgaufs_init(void);
#else
AuStubVoid(dbgaufs_xino_del, struct au_branch *br)
AuStubVoid(dbgaufs_brs_del, struct super_block *sb, aufs_bindex_t bindex)
AuStubVoid(dbgaufs_brs_add, struct super_block *sb, aufs_bindex_t bindex,
	   int topdown)
AuStubVoid(dbgaufs_si_fin, struct au_sbinfo *sbinfo)
AuStubInt0(dbgaufs_si_init, struct au_sbinfo *sbinfo)
AuStubVoid(dbgaufs_fin, void)
AuStubInt0(__init dbgaufs_init, void)
#endif /* CONFIG_DEBUG_FS */

#endif /* __KERNEL__ */
#endif /* __DBGAUFS_H__ */
