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
 * pseudo-link
 */

#include "aufs.h"

/*
 * the pseudo-link maintenance mode.
 * during a user process maintains the pseudo-links,
 * prohibit adding a new plink and branch manipulation.
 *
 * Flags
 * NOPLM:
 *	For entry functions which will handle plink, and i_mutex is already held
 *	in VFS.
 *	They cannot wait and should return an error at once.
 *	Callers has to check the error.
 * NOPLMW:
 *	For entry functions which will handle plink, but i_mutex is not held
 *	in VFS.
 *	They can wait the plink maintenance mode to finish.
 *
 * They behave like F_SETLK and F_SETLKW.
 * If the caller never handle plink, then both flags are unnecessary.
 */

int au_plink_maint(struct super_block *sb, int flags)
{
	int err;
	pid_t pid, ppid;
	struct au_sbinfo *sbi;

	SiMustAnyLock(sb);

	err = 0;
	if (!au_opt_test(au_mntflags(sb), PLINK))
		goto out;

	sbi = au_sbi(sb);
	pid = sbi->si_plink_maint_pid;
	if (!pid || pid == current->pid)
		goto out;

	/* todo: it highly depends upon /sbin/mount.aufs */
	rcu_read_lock();
	ppid = task_pid_vnr(rcu_dereference(current->real_parent));
	rcu_read_unlock();
	if (pid == ppid)
		goto out;

	if (au_ftest_lock(flags, NOPLMW)) {
		/* if there is no i_mutex lock in VFS, we don't need to wait */
		/* AuDebugOn(!lockdep_depth(current)); */
		while (sbi->si_plink_maint_pid) {
			si_read_unlock(sb);
			/* gave up wake_up_bit() */
			wait_event(sbi->si_plink_wq, !sbi->si_plink_maint_pid);

			if (au_ftest_lock(flags, FLUSH))
				au_nwt_flush(&sbi->si_nowait);
			si_noflush_read_lock(sb);
		}
	} else if (au_ftest_lock(flags, NOPLM)) {
		AuDbg("ppid %d, pid %d\n", ppid, pid);
		err = -EAGAIN;
	}

out:
	return err;
}

void au_plink_maint_leave(struct au_sbinfo *sbinfo)
{
	spin_lock(&sbinfo->si_plink_maint_lock);
	sbinfo->si_plink_maint_pid = 0;
	spin_unlock(&sbinfo->si_plink_maint_lock);
	wake_up_all(&sbinfo->si_plink_wq);
}

int au_plink_maint_enter(struct super_block *sb)
{
	int err;
	struct au_sbinfo *sbinfo;

	err = 0;
	sbinfo = au_sbi(sb);
	/* make sure i am the only one in this fs */
	si_write_lock(sb, AuLock_FLUSH);
	if (au_opt_test(au_mntflags(sb), PLINK)) {
		spin_lock(&sbinfo->si_plink_maint_lock);
		if (!sbinfo->si_plink_maint_pid)
			sbinfo->si_plink_maint_pid = current->pid;
		else
			err = -EBUSY;
		spin_unlock(&sbinfo->si_plink_maint_lock);
	}
	si_write_unlock(sb);

	return err;
}

/* ---------------------------------------------------------------------- */

#ifdef CONFIG_AUFS_DEBUG
void au_plink_list(struct super_block *sb)
{
	int i;
	struct au_sbinfo *sbinfo;
	struct hlist_head *plink_hlist;
	struct pseudo_link *plink;

	SiMustAnyLock(sb);

	sbinfo = au_sbi(sb);
	AuDebugOn(!au_opt_test(au_mntflags(sb), PLINK));
	AuDebugOn(au_plink_maint(sb, AuLock_NOPLM));

	for (i = 0; i < AuPlink_NHASH; i++) {
		plink_hlist = &sbinfo->si_plink[i].head;
		rcu_read_lock();
		hlist_for_each_entry_rcu(plink, plink_hlist, hlist)
			AuDbg("%lu\n", plink->inode->i_ino);
		rcu_read_unlock();
	}
}
#endif

/* is the inode pseudo-linked? */
int au_plink_test(struct inode *inode)
{
	int found, i;
	struct au_sbinfo *sbinfo;
	struct hlist_head *plink_hlist;
	struct pseudo_link *plink;

	sbinfo = au_sbi(inode->i_sb);
	AuRwMustAnyLock(&sbinfo->si_rwsem);
	AuDebugOn(!au_opt_test(au_mntflags(inode->i_sb), PLINK));
	AuDebugOn(au_plink_maint(inode->i_sb, AuLock_NOPLM));

	found = 0;
	i = au_plink_hash(inode->i_ino);
	plink_hlist = &sbinfo->si_plink[i].head;
	rcu_read_lock();
	hlist_for_each_entry_rcu(plink, plink_hlist, hlist)
		if (plink->inode == inode) {
			found = 1;
			break;
		}
	rcu_read_unlock();
	return found;
}

/* ---------------------------------------------------------------------- */

/*
 * generate a name for plink.
 * the file will be stored under AUFS_WH_PLINKDIR.
 */
/* 20 is max digits length of ulong 64 */
#define PLINK_NAME_LEN	((20 + 1) * 2)

static int plink_name(char *name, int len, struct inode *inode,
		      aufs_bindex_t bindex)
{
	int rlen;
	struct inode *h_inode;

	h_inode = au_h_iptr(inode, bindex);
	rlen = snprintf(name, len, "%lu.%lu", inode->i_ino, h_inode->i_ino);
	return rlen;
}

struct au_do_plink_lkup_args {
	struct dentry **errp;
	struct qstr *tgtname;
	struct dentry *h_parent;
	struct au_branch *br;
};

static struct dentry *au_do_plink_lkup(struct qstr *tgtname,
				       struct dentry *h_parent,
				       struct au_branch *br)
{
	struct dentry *h_dentry;
	struct mutex *h_mtx;

	h_mtx = &h_parent->d_inode->i_mutex;
	mutex_lock_nested(h_mtx, AuLsc_I_CHILD2);
	h_dentry = vfsub_lkup_one(tgtname, h_parent);
	mutex_unlock(h_mtx);
	return h_dentry;
}

static void au_call_do_plink_lkup(void *args)
{
	struct au_do_plink_lkup_args *a = args;
	*a->errp = au_do_plink_lkup(a->tgtname, a->h_parent, a->br);
}

/* create a pseudo-link */
static int do_whplink(struct qstr *tgt, struct dentry *h_parent,
		      struct dentry *h_dentry, struct au_branch *br)
{
	int err;
	struct path h_path = {
		.mnt = au_br_mnt(br)
	};
	struct inode *h_dir, *delegated;

	h_dir = h_parent->d_inode;
	mutex_lock_nested(&h_dir->i_mutex, AuLsc_I_CHILD2);
again:
	h_path.dentry = vfsub_lkup_one(tgt, h_parent);
	err = PTR_ERR(h_path.dentry);
	if (IS_ERR(h_path.dentry))
		goto out;

	err = 0;
	/* wh.plink dir is not monitored */
	/* todo: is it really safe? */
	if (h_path.dentry->d_inode
	    && h_path.dentry->d_inode != h_dentry->d_inode) {
		delegated = NULL;
		err = vfsub_unlink(h_dir, &h_path, &delegated, /*force*/0);
		if (unlikely(err == -EWOULDBLOCK)) {
			pr_warn("cannot retry for NFSv4 delegation"
				" for an internal unlink\n");
			iput(delegated);
		}
		dput(h_path.dentry);
		h_path.dentry = NULL;
		if (!err)
			goto again;
	}
	if (!err && !h_path.dentry->d_inode) {
		delegated = NULL;
		err = vfsub_link(h_dentry, h_dir, &h_path, &delegated);
		if (unlikely(err == -EWOULDBLOCK)) {
			pr_warn("cannot retry for NFSv4 delegation"
				" for an internal link\n");
			iput(delegated);
		}
	}
	dput(h_path.dentry);

out:
	mutex_unlock(&h_dir->i_mutex);
	return err;
}

struct do_whplink_args {
	int *errp;
	struct qstr *tgt;
	struct dentry *h_parent;
	struct dentry *h_dentry;
	struct au_branch *br;
};

static void call_do_whplink(void *args)
{
	struct do_whplink_args *a = args;
	*a->errp = do_whplink(a->tgt, a->h_parent, a->h_dentry, a->br);
}

/* free a single plink */
static void do_put_plink(struct pseudo_link *plink, int do_del)
{
	if (do_del)
		hlist_del(&plink->hlist);
	iput(plink->inode);
	kfree(plink);
}

static void do_put_plink_rcu(struct rcu_head *rcu)
{
	struct pseudo_link *plink;

	plink = container_of(rcu, struct pseudo_link, rcu);
	iput(plink->inode);
	kfree(plink);
}

/*
 * create a new pseudo-link for @h_dentry on @bindex.
 * the linked inode is held in aufs @inode.
 */
void au_plink_append(struct inode *inode, aufs_bindex_t bindex,
		     struct dentry *h_dentry)
{
	struct super_block *sb;
	struct au_sbinfo *sbinfo;
	struct hlist_head *plink_hlist;
	struct pseudo_link *plink, *tmp;
	struct au_sphlhead *sphl;
	int found, err, cnt, i;

	sb = inode->i_sb;
	sbinfo = au_sbi(sb);
	AuDebugOn(!au_opt_test(au_mntflags(sb), PLINK));
	AuDebugOn(au_plink_maint(sb, AuLock_NOPLM));

	found = au_plink_test(inode);
	if (found)
		return;

	i = au_plink_hash(inode->i_ino);
	sphl = sbinfo->si_plink + i;
	plink_hlist = &sphl->head;
	tmp = kmalloc(sizeof(*plink), GFP_NOFS);
	if (tmp)
		tmp->inode = au_igrab(inode);
	else {
		err = -ENOMEM;
		goto out;
	}

	spin_lock(&sphl->spin);
	hlist_for_each_entry(plink, plink_hlist, hlist) {
		if (plink->inode == inode) {
			found = 1;
			break;
		}
	}
	if (!found)
		hlist_add_head_rcu(&tmp->hlist, plink_hlist);
	spin_unlock(&sphl->spin);
	if (!found) {
		cnt = au_sphl_count(sphl);
#define msg "unexpectedly unblanced or too many pseudo-links"
		if (cnt > AUFS_PLINK_WARN)
			AuWarn1(msg ", %d\n", cnt);
#undef msg
		/* err = whplink(h_dentry, inode, bindex, au_sbr(sb, bindex)); re-commit later */
		err = 0;
	} else {
		do_put_plink(tmp, 0);
		return;
	}

out:
	if (unlikely(err)) {
		pr_warn("err %d, damaged pseudo link.\n", err);
		if (tmp) {
			au_sphl_del_rcu(&tmp->hlist, sphl);
			call_rcu(&tmp->rcu, do_put_plink_rcu);
		}
	}
}

/* free all plinks */
void au_plink_put(struct super_block *sb, int verbose)
{
	int i, warned;
	struct au_sbinfo *sbinfo;
	struct hlist_head *plink_hlist;
	struct hlist_node *tmp;
	struct pseudo_link *plink;

	SiMustWriteLock(sb);

	sbinfo = au_sbi(sb);
	AuDebugOn(!au_opt_test(au_mntflags(sb), PLINK));
	AuDebugOn(au_plink_maint(sb, AuLock_NOPLM));

	/* no spin_lock since sbinfo is write-locked */
	warned = 0;
	for (i = 0; i < AuPlink_NHASH; i++) {
		plink_hlist = &sbinfo->si_plink[i].head;
		if (!warned && verbose && !hlist_empty(plink_hlist)) {
			pr_warn("pseudo-link is not flushed");
			warned = 1;
		}
		hlist_for_each_entry_safe(plink, tmp, plink_hlist, hlist)
			do_put_plink(plink, 0);
		INIT_HLIST_HEAD(plink_hlist);
	}
}

void au_plink_clean(struct super_block *sb, int verbose)
{
	si_write_lock(sb, AuLock_FLUSH | AuLock_NOPLMW); /* replaced later */
	if (au_opt_test(au_mntflags(sb), PLINK))
		au_plink_put(sb, verbose);
	si_write_unlock(sb);
}
