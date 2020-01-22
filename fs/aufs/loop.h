/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2005-2020 Junjiro R. Okajima
 */

/*
 * support for loopback mount as a branch
 */

#ifndef __AUFS_LOOP_H__
#define __AUFS_LOOP_H__

#ifdef __KERNEL__

struct dentry;
struct super_block;

#ifdef CONFIG_AUFS_BDEV_LOOP
/* drivers/block/loop.c */
struct file *loop_backing_file(struct super_block *sb);

/* loop.c */
int au_test_loopback_overlap(struct super_block *sb, struct dentry *h_adding);
int au_test_loopback_kthread(void);
void au_warn_loopback(struct super_block *h_sb);

int au_loopback_init(void);
void au_loopback_fin(void);
#else
AuStub(struct file *, loop_backing_file, return NULL, struct super_block *sb)

AuStubInt0(au_test_loopback_overlap, struct super_block *sb,
	   struct dentry *h_adding)
AuStubInt0(au_test_loopback_kthread, void)
AuStubVoid(au_warn_loopback, struct super_block *h_sb)

AuStubInt0(au_loopback_init, void)
AuStubVoid(au_loopback_fin, void)
#endif /* BLK_DEV_LOOP */

#endif /* __KERNEL__ */
#endif /* __AUFS_LOOP_H__ */
