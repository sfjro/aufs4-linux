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
 * super_block operations
 */

#ifndef __AUFS_SUPER_H__
#define __AUFS_SUPER_H__

#ifdef __KERNEL__

#include <linux/fs.h>
#include <linux/kobject.h>
#include "rwsem.h"
#include "spl.h"
#include "wkq.h"

typedef ssize_t (*au_readf_t)(struct file *, char __user *, size_t, loff_t *);
typedef ssize_t (*au_writef_t)(struct file *, const char __user *, size_t,
			       loff_t *);

struct pseudo_link {
	union {
		struct hlist_node hlist;
		struct rcu_head rcu;
	};
	struct inode *inode;
};

#define AuPlink_NHASH 100
static inline int au_plink_hash(ino_t ino)
{
	return ino % AuPlink_NHASH;
}

struct au_branch;
struct au_sbinfo {
	/* nowait tasks in the system-wide workqueue */
	struct au_nowait_tasks	si_nowait;

	/*
	 * tried sb->s_umount, but failed due to the dependecy between i_mutex.
	 * rwsem for au_sbinfo is necessary.
	 */
	struct au_rwsem		si_rwsem;

	/* prevent recursive locking in deleting inode */
	struct {
		unsigned long		*bitmap;
		spinlock_t		tree_lock;
		struct radix_tree_root	tree;
	} au_si_pid;

	/* branch management */
	unsigned int		si_generation;

	aufs_bindex_t		si_bend;

	/* dirty trick to keep br_id plus */
	unsigned int		si_last_br_id :
				sizeof(aufs_bindex_t) * BITS_PER_BYTE - 1;
	struct au_branch	**si_branch;

	/* mount flags */
	/* include/asm-ia64/siginfo.h defines a macro named si_flags */
	unsigned int		si_mntflags;

	/* external inode number (bitmap and translation table) */
	au_readf_t		si_xread;
	au_writef_t		si_xwrite;
	struct file		*si_xib;
	struct mutex		si_xib_mtx; /* protect xib members */
	unsigned long		*si_xib_buf;
	unsigned long		si_xib_last_pindex;
	int			si_xib_next_bit;
	aufs_bindex_t		si_xino_brid;
	/* reserved for future use */
	/* unsigned long long	si_xib_limit; */	/* Max xib file size */

	/* pseudo_link list */
	struct au_sphlhead	si_plink[AuPlink_NHASH];
	wait_queue_head_t	si_plink_wq;
	spinlock_t		si_plink_maint_lock;
	pid_t			si_plink_maint_pid;

	/*
	 * sysfs and lifetime management.
	 * this is not a small structure and it may be a waste of memory in case
	 * of sysfs is disabled, particulary when many aufs-es are mounted.
	 * but using sysfs is majority.
	 */
	struct kobject		si_kobj;

#ifdef CONFIG_AUFS_SBILIST
	struct list_head	si_list;
#endif

	/* dirty, necessary for unmounting, sysfs and sysrq */
	struct super_block	*si_sb;
};

/* ---------------------------------------------------------------------- */

/* flags for si_read_lock()/aufs_read_lock()/di_read_lock() */
#define AuLock_DW		1		/* write-lock dentry */
#define AuLock_IR		(1 << 1)	/* read-lock inode */
#define AuLock_IW		(1 << 2)	/* write-lock inode */
#define AuLock_FLUSH		(1 << 3)	/* wait for 'nowait' tasks */
#define AuLock_NOPLM		(1 << 5)	/* return err in plm mode */
#define AuLock_NOPLMW		(1 << 6)	/* wait for plm mode ends */
#define AuLock_GEN		(1 << 7)	/* test digen/iigen */
#define au_ftest_lock(flags, name)	((flags) & AuLock_##name)
#define au_fset_lock(flags, name) \
	do { (flags) |= AuLock_##name; } while (0)
#define au_fclr_lock(flags, name) \
	do { (flags) &= ~AuLock_##name; } while (0)

/* ---------------------------------------------------------------------- */

/* super.c */
extern struct file_system_type aufs_fs_type;
struct inode *au_iget_locked(struct super_block *sb, ino_t ino);

/* sbinfo.c */
void au_si_free(struct kobject *kobj);
int au_si_alloc(struct super_block *sb);
int au_sbr_realloc(struct au_sbinfo *sbinfo, int nbr);

unsigned int au_sigen_inc(struct super_block *sb);
aufs_bindex_t au_new_br_id(struct super_block *sb);

int si_read_lock(struct super_block *sb, int flags);
int si_write_lock(struct super_block *sb, int flags);
int aufs_read_lock(struct dentry *dentry, int flags);
void aufs_read_unlock(struct dentry *dentry, int flags);
void aufs_write_lock(struct dentry *dentry);
void aufs_write_unlock(struct dentry *dentry);

int si_pid_test_slow(struct super_block *sb);
void si_pid_set_slow(struct super_block *sb);
void si_pid_clr_slow(struct super_block *sb);

/* ---------------------------------------------------------------------- */

static inline struct au_sbinfo *au_sbi(struct super_block *sb)
{
	return sb->s_fs_info;
}

/* ---------------------------------------------------------------------- */

#ifdef CONFIG_AUFS_SBILIST
/* module.c */
extern struct au_splhead au_sbilist;

static inline void au_sbilist_init(void)
{
	au_spl_init(&au_sbilist);
}

static inline void au_sbilist_add(struct super_block *sb)
{
	au_spl_add(&au_sbi(sb)->si_list, &au_sbilist);
}

static inline void au_sbilist_del(struct super_block *sb)
{
	au_spl_del(&au_sbi(sb)->si_list, &au_sbilist);
}

#define AuGFP_SBILIST	GFP_NOFS
#else
AuStubVoid(au_sbilist_init, void)
AuStubVoid(au_sbilist_add, struct super_block *sb)
AuStubVoid(au_sbilist_del, struct super_block *sb)
#define AuGFP_SBILIST	GFP_NOFS
#endif

/* ---------------------------------------------------------------------- */

static inline pid_t si_pid_bit(void)
{
	/* the origin of pid is 1, but the bitmap's is 0 */
	return current->pid - 1;
}

static inline int si_pid_test(struct super_block *sb)
{
	pid_t bit;

	bit = si_pid_bit();
	if (bit < PID_MAX_DEFAULT)
		return test_bit(bit, au_sbi(sb)->au_si_pid.bitmap);
	return si_pid_test_slow(sb);
}

static inline void si_pid_set(struct super_block *sb)
{
	pid_t bit;

	bit = si_pid_bit();
	if (bit < PID_MAX_DEFAULT) {
		AuDebugOn(test_bit(bit, au_sbi(sb)->au_si_pid.bitmap));
		set_bit(bit, au_sbi(sb)->au_si_pid.bitmap);
		/* smp_mb(); */
	} else
		si_pid_set_slow(sb);
}

static inline void si_pid_clr(struct super_block *sb)
{
	pid_t bit;

	bit = si_pid_bit();
	if (bit < PID_MAX_DEFAULT) {
		AuDebugOn(!test_bit(bit, au_sbi(sb)->au_si_pid.bitmap));
		clear_bit(bit, au_sbi(sb)->au_si_pid.bitmap);
		/* smp_mb(); */
	} else
		si_pid_clr_slow(sb);
}

/* ---------------------------------------------------------------------- */

/* lock superblock. mainly for entry point functions */
/*
 * __si_read_lock, __si_write_lock,
 * __si_read_unlock, __si_write_unlock, __si_downgrade_lock
 */
AuSimpleRwsemFuncs(__si, struct super_block *sb, &au_sbi(sb)->si_rwsem);

#define SiMustNoWaiters(sb)	AuRwMustNoWaiters(&au_sbi(sb)->si_rwsem)
#define SiMustAnyLock(sb)	AuRwMustAnyLock(&au_sbi(sb)->si_rwsem)
#define SiMustWriteLock(sb)	AuRwMustWriteLock(&au_sbi(sb)->si_rwsem)

static inline void si_noflush_read_lock(struct super_block *sb)
{
	__si_read_lock(sb);
	si_pid_set(sb);
}

static inline int si_noflush_read_trylock(struct super_block *sb)
{
	int locked;

	locked = __si_read_trylock(sb);
	if (locked)
		si_pid_set(sb);
	return locked;
}

static inline void si_noflush_write_lock(struct super_block *sb)
{
	__si_write_lock(sb);
	si_pid_set(sb);
}

static inline int si_noflush_write_trylock(struct super_block *sb)
{
	int locked;

	locked = __si_write_trylock(sb);
	if (locked)
		si_pid_set(sb);
	return locked;
}

#if 0 /* reserved */
static inline int si_read_trylock(struct super_block *sb, int flags)
{
	if (au_ftest_lock(flags, FLUSH))
		au_nwt_flush(&au_sbi(sb)->si_nowait);
	return si_noflush_read_trylock(sb);
}
#endif

static inline void si_read_unlock(struct super_block *sb)
{
	si_pid_clr(sb);
	__si_read_unlock(sb);
}

#if 0 /* reserved */
static inline int si_write_trylock(struct super_block *sb, int flags)
{
	if (au_ftest_lock(flags, FLUSH))
		au_nwt_flush(&au_sbi(sb)->si_nowait);
	return si_noflush_write_trylock(sb);
}
#endif

static inline void si_write_unlock(struct super_block *sb)
{
	si_pid_clr(sb);
	__si_write_unlock(sb);
}

#if 0 /* reserved */
static inline void si_downgrade_lock(struct super_block *sb)
{
	__si_downgrade_lock(sb);
}
#endif

/* ---------------------------------------------------------------------- */

static inline aufs_bindex_t au_sbend(struct super_block *sb)
{
	SiMustAnyLock(sb);
	return au_sbi(sb)->si_bend;
}

static inline unsigned int au_mntflags(struct super_block *sb)
{
	SiMustAnyLock(sb);
	return au_sbi(sb)->si_mntflags;
}

static inline unsigned int au_sigen(struct super_block *sb)
{
	SiMustAnyLock(sb);
	return au_sbi(sb)->si_generation;
}

static inline struct au_branch *au_sbr(struct super_block *sb,
				       aufs_bindex_t bindex)
{
	SiMustAnyLock(sb);
	return au_sbi(sb)->si_branch[0 + bindex];
}

static inline void au_xino_brid_set(struct super_block *sb, aufs_bindex_t brid)
{
	SiMustWriteLock(sb);
	au_sbi(sb)->si_xino_brid = brid;
}

static inline aufs_bindex_t au_xino_brid(struct super_block *sb)
{
	SiMustAnyLock(sb);
	return au_sbi(sb)->si_xino_brid;
}

#endif /* __KERNEL__ */
#endif /* __AUFS_SUPER_H__ */
