/*
 * Copyright (C) 2005-2018 Junjiro R. Okajima
 */

/*
 * branch filesystems and xino for them
 */

#ifndef __AUFS_BRANCH_H__
#define __AUFS_BRANCH_H__

#ifdef __KERNEL__

#include <linux/mount.h>
#include "dirren.h"
#include "dynop.h"
#include "rwsem.h"
#include "super.h"

/* ---------------------------------------------------------------------- */

/* a xino file */
struct au_xino {
	struct file		*xi_file;
	struct {
		spinlock_t		spin;
		ino_t			*array;
		int			total;
		/* reserved for future use */
		/* unsigned long	*bitmap; */
		wait_queue_head_t	wqh;
	} xi_nondir;

	atomic_t		xi_truncating;

	struct kref		xi_kref;

	/* todo: make xino files an array to support huge inode number */
};

/* File-based Hierarchical Storage Management */
struct au_br_fhsm {
#ifdef CONFIG_AUFS_FHSM
	struct mutex		bf_lock;
	unsigned long		bf_jiffy;
	struct aufs_stfs	bf_stfs;
	int			bf_readable;
#endif
};

/* members for writable branch only */
enum {AuBrWh_BASE, AuBrWh_PLINK, AuBrWh_ORPH, AuBrWh_Last};
struct au_wbr {
	struct au_rwsem		wbr_wh_rwsem;
	struct dentry		*wbr_wh[AuBrWh_Last];
	atomic_t		wbr_wh_running;
#define wbr_whbase		wbr_wh[AuBrWh_BASE]	/* whiteout base */
#define wbr_plink		wbr_wh[AuBrWh_PLINK]	/* pseudo-link dir */
#define wbr_orph		wbr_wh[AuBrWh_ORPH]	/* dir for orphans */

	/* mfs mode */
	unsigned long long	wbr_bytes;
};

/* ext2 has 3 types of operations at least, ext3 has 4 */
#define AuBrDynOp (AuDyLast * 4)

#ifdef CONFIG_AUFS_HFSNOTIFY
/* support for asynchronous destruction */
struct au_br_hfsnotify {
	struct fsnotify_group	*hfsn_group;
};
#endif

/* sysfs entries */
struct au_brsysfs {
	char			name[16];
	struct attribute	attr;
};

enum {
	AuBrSysfs_BR,
	AuBrSysfs_BRID,
	AuBrSysfs_Last
};

/* protected by superblock rwsem */
struct au_branch {
	struct au_xino		*br_xino;

	aufs_bindex_t		br_id;

	int			br_perm;
	struct path		br_path;
	spinlock_t		br_dykey_lock;
	struct au_dykey		*br_dykey[AuBrDynOp];
	struct percpu_counter	br_count;

	struct au_wbr		*br_wbr;
	struct au_br_fhsm	*br_fhsm;

#ifdef CONFIG_AUFS_HFSNOTIFY
	struct au_br_hfsnotify	*br_hfsn;
#endif

#ifdef CONFIG_SYSFS
	/* entries under sysfs per mount-point */
	struct au_brsysfs	br_sysfs[AuBrSysfs_Last];
#endif

#ifdef CONFIG_DEBUG_FS
	struct dentry		 *br_dbgaufs; /* xino */
#endif

	struct au_dr_br		br_dirren;
};

/* ---------------------------------------------------------------------- */

static inline struct vfsmount *au_br_mnt(struct au_branch *br)
{
	return br->br_path.mnt;
}

static inline struct dentry *au_br_dentry(struct au_branch *br)
{
	return br->br_path.dentry;
}

static inline struct super_block *au_br_sb(struct au_branch *br)
{
	return au_br_mnt(br)->mnt_sb;
}

static inline void au_br_get(struct au_branch *br)
{
	percpu_counter_inc(&br->br_count);
}

static inline void au_br_put(struct au_branch *br)
{
	percpu_counter_dec(&br->br_count);
}

static inline s64 au_br_count(struct au_branch *br)
{
	return percpu_counter_sum(&br->br_count);
}

static inline void au_br_count_init(struct au_branch *br)
{
	percpu_counter_init(&br->br_count, 0, GFP_NOFS);
}

static inline void au_br_count_fin(struct au_branch *br)
{
	percpu_counter_destroy(&br->br_count);
}

static inline int au_br_rdonly(struct au_branch *br)
{
	return ((au_br_sb(br)->s_flags & MS_RDONLY)
		|| !au_br_writable(br->br_perm))
		? -EROFS : 0;
}

static inline int au_br_hnotifyable(int brperm __maybe_unused)
{
#ifdef CONFIG_AUFS_HNOTIFY
	return !(brperm & AuBrPerm_RR);
#else
	return 0;
#endif
}

static inline int au_br_test_oflag(int oflag, struct au_branch *br)
{
	int err, exec_flag;

	err = 0;
	exec_flag = oflag & __FMODE_EXEC;
	if (unlikely(exec_flag && path_noexec(&br->br_path)))
		err = -EACCES;

	return err;
}

static inline void au_xino_get(struct au_branch *br)
{
	struct au_xino *xi;

	xi = br->br_xino;
	if (xi)
		kref_get(&xi->xi_kref);
}

static inline int au_xino_count(struct au_branch *br)
{
	int v;
	struct au_xino *xi;

	v = 0;
	xi = br->br_xino;
	if (xi)
		v = kref_read(&xi->xi_kref);

	return v;
}

static inline struct file *au_xino_file(struct au_branch *br)
{
	struct au_xino *xi;

	xi = br->br_xino;
	return xi ? xi->xi_file : NULL;
}

/* ---------------------------------------------------------------------- */

/* branch.c */
struct au_sbinfo;
void au_br_free(struct au_sbinfo *sinfo);
int au_br_index(struct super_block *sb, aufs_bindex_t br_id);
struct au_opt_add;
int au_br_add(struct super_block *sb, struct au_opt_add *add, int remount);
struct au_opt_del;
int au_br_del(struct super_block *sb, struct au_opt_del *del, int remount);
long au_ibusy_ioctl(struct file *file, unsigned long arg);
#ifdef CONFIG_COMPAT
long au_ibusy_compat_ioctl(struct file *file, unsigned long arg);
#endif
struct au_opt_mod;
int au_br_mod(struct super_block *sb, struct au_opt_mod *mod, int remount,
	      int *do_refresh);
struct aufs_stfs;
int au_br_stfs(struct au_branch *br, struct aufs_stfs *stfs);

/* xino.c */
static const loff_t au_loff_max = LLONG_MAX;

struct file *au_xino_create(struct super_block *sb, char *fpath, int silent);
struct file *au_xino_create2(struct super_block *sb, struct path *base,
			     struct file *copy_src);

int au_xino_read(struct super_block *sb, aufs_bindex_t bindex, ino_t h_ino,
		 ino_t *ino);
int au_xino_write(struct super_block *sb, aufs_bindex_t bindex, ino_t h_ino,
		  ino_t ino);
ssize_t xino_fread(vfs_readf_t func, struct file *file, void *buf, size_t size,
		   loff_t *pos);
ssize_t xino_fwrite(vfs_writef_t func, struct file *file, void *buf,
		    size_t size, loff_t *pos);

int au_xib_trunc(struct super_block *sb);
int au_xino_trunc(struct super_block *sb, aufs_bindex_t bindex);

struct au_xino *au_xino_alloc(void);
int au_xino_put(struct au_branch *br);
void au_xino_file_set(struct au_branch *br, struct file *file);

struct au_opt_xino;
void au_xino_clr(struct super_block *sb);
int au_xino_set(struct super_block *sb, struct au_opt_xino *xiopt, int remount);
struct file *au_xino_def(struct super_block *sb);
int au_xino_init_br(struct super_block *sb, struct au_branch *br, ino_t hino,
		    struct path *base);

ino_t au_xino_new_ino(struct super_block *sb);
void au_xino_delete_inode(struct inode *inode, const int unlinked);

void au_xinondir_leave(struct super_block *sb, aufs_bindex_t bindex,
		       ino_t h_ino, int idx);
int au_xinondir_enter(struct super_block *sb, aufs_bindex_t bindex, ino_t h_ino,
		      int *idx);

int au_xino_path(struct seq_file *seq, struct file *file);

/* ---------------------------------------------------------------------- */

/* Superblock to branch */
static inline
aufs_bindex_t au_sbr_id(struct super_block *sb, aufs_bindex_t bindex)
{
	return au_sbr(sb, bindex)->br_id;
}

static inline
struct vfsmount *au_sbr_mnt(struct super_block *sb, aufs_bindex_t bindex)
{
	return au_br_mnt(au_sbr(sb, bindex));
}

static inline
struct super_block *au_sbr_sb(struct super_block *sb, aufs_bindex_t bindex)
{
	return au_br_sb(au_sbr(sb, bindex));
}

static inline void au_sbr_get(struct super_block *sb, aufs_bindex_t bindex)
{
	au_br_get(au_sbr(sb, bindex));
}

static inline void au_sbr_put(struct super_block *sb, aufs_bindex_t bindex)
{
	au_br_put(au_sbr(sb, bindex));
}

static inline int au_sbr_perm(struct super_block *sb, aufs_bindex_t bindex)
{
	return au_sbr(sb, bindex)->br_perm;
}

static inline int au_sbr_whable(struct super_block *sb, aufs_bindex_t bindex)
{
	return au_br_whable(au_sbr_perm(sb, bindex));
}

/* ---------------------------------------------------------------------- */

/*
 * wbr_wh_read_lock, wbr_wh_write_lock
 * wbr_wh_read_unlock, wbr_wh_write_unlock, wbr_wh_downgrade_lock
 */
AuSimpleRwsemFuncs(wbr_wh, struct au_wbr *wbr, &wbr->wbr_wh_rwsem);

#define WbrWhMustNoWaiters(wbr)	AuRwMustNoWaiters(&wbr->wbr_wh_rwsem)
#define WbrWhMustAnyLock(wbr)	AuRwMustAnyLock(&wbr->wbr_wh_rwsem)
#define WbrWhMustWriteLock(wbr)	AuRwMustWriteLock(&wbr->wbr_wh_rwsem)

/* ---------------------------------------------------------------------- */

#ifdef CONFIG_AUFS_FHSM
static inline void au_br_fhsm_init(struct au_br_fhsm *brfhsm)
{
	mutex_init(&brfhsm->bf_lock);
	brfhsm->bf_jiffy = 0;
	brfhsm->bf_readable = 0;
}

static inline void au_br_fhsm_fin(struct au_br_fhsm *brfhsm)
{
	mutex_destroy(&brfhsm->bf_lock);
}
#else
AuStubVoid(au_br_fhsm_init, struct au_br_fhsm *brfhsm)
AuStubVoid(au_br_fhsm_fin, struct au_br_fhsm *brfhsm)
#endif

#endif /* __KERNEL__ */
#endif /* __AUFS_BRANCH_H__ */
