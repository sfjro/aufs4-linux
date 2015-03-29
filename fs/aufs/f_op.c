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
 * file and vm operations
 */

#include <linux/fs_stack.h>
#include <linux/mman.h>
#include "aufs.h"

int au_do_open_nondir(struct file *file, int flags)
{
	int err;
	aufs_bindex_t bindex;
	struct file *h_file;
	struct dentry *dentry;
	struct au_finfo *finfo;
	struct inode *h_inode;

	FiMustWriteLock(file);

	err = 0;
	dentry = file->f_path.dentry;
	finfo = au_fi(file);
	memset(&finfo->fi_htop, 0, sizeof(finfo->fi_htop));
	atomic_set(&finfo->fi_mmapped, 0);
	bindex = au_dbstart(dentry);
	h_file = au_h_open(dentry, bindex, flags, file);
	if (IS_ERR(h_file))
		err = PTR_ERR(h_file);
	else {
		if ((flags & __O_TMPFILE)
		    && !(flags & O_EXCL)) {
			h_inode = file_inode(h_file);
			spin_lock(&h_inode->i_lock);
			h_inode->i_state |= I_LINKABLE;
			spin_unlock(&h_inode->i_lock);
		}
		au_set_fbstart(file, bindex);
		au_set_h_fptr(file, bindex, h_file);
		au_update_figen(file);
		/* todo: necessary? */
		/* file->f_ra = h_file->f_ra; */
	}

	return err;
}

static int aufs_open_nondir(struct inode *inode __maybe_unused,
			    struct file *file)
{
	int err;
	struct super_block *sb;

	AuDbg("%pD, f_flags 0x%x, f_mode 0x%x\n",
	      file, vfsub_file_flags(file), file->f_mode);

	sb = file->f_path.dentry->d_sb;
	si_read_lock(sb, AuLock_FLUSH);
	err = au_do_open(file, au_do_open_nondir, /*fidir*/NULL);
	si_read_unlock(sb);
	return err;
}

int aufs_release_nondir(struct inode *inode __maybe_unused, struct file *file)
{
	struct au_finfo *finfo;
	aufs_bindex_t bindex;

	finfo = au_fi(file);
	bindex = finfo->fi_btop;
	if (bindex >= 0)
		au_set_h_fptr(file, bindex, NULL);

	au_finfo_fin(file);
	return 0;
}

/* ---------------------------------------------------------------------- */

/*
 * The locking order around current->mmap_sem.
 * - in most and regular cases
 *   file I/O syscall -- aufs_read() or something
 *	-- si_rwsem for read -- mmap_sem
 *	(Note that [fdi]i_rwsem are released before mmap_sem).
 * - in mmap case
 *   mmap(2) -- mmap_sem -- aufs_mmap() -- si_rwsem for read -- [fdi]i_rwsem
 * This AB-BA order is definitly bad, but is not a problem since "si_rwsem for
 * read" allows muliple processes to acquire it and [fdi]i_rwsem are not held in
 * file I/O. Aufs needs to stop lockdep in aufs_mmap() though.
 * It means that when aufs acquires si_rwsem for write, the process should never
 * acquire mmap_sem.
 *
 * Actually aufs_iterate() holds [fdi]i_rwsem before mmap_sem, but this is not a
 * problem either since any directory is not able to be mmap-ed.
 * The similar scenario is applied to aufs_readlink() too.
 */

#if 0 /* stop calling security_file_mmap() */
/* cf. linux/include/linux/mman.h: calc_vm_prot_bits() */
#define AuConv_VM_PROT(f, b)	_calc_vm_trans(f, VM_##b, PROT_##b)

static unsigned long au_arch_prot_conv(unsigned long flags)
{
	/* currently ppc64 only */
#ifdef CONFIG_PPC64
	/* cf. linux/arch/powerpc/include/asm/mman.h */
	AuDebugOn(arch_calc_vm_prot_bits(-1) != VM_SAO);
	return AuConv_VM_PROT(flags, SAO);
#else
	AuDebugOn(arch_calc_vm_prot_bits(-1));
	return 0;
#endif
}

static unsigned long au_prot_conv(unsigned long flags)
{
	return AuConv_VM_PROT(flags, READ)
		| AuConv_VM_PROT(flags, WRITE)
		| AuConv_VM_PROT(flags, EXEC)
		| au_arch_prot_conv(flags);
}

/* cf. linux/include/linux/mman.h: calc_vm_flag_bits() */
#define AuConv_VM_MAP(f, b)	_calc_vm_trans(f, VM_##b, MAP_##b)

static unsigned long au_flag_conv(unsigned long flags)
{
	return AuConv_VM_MAP(flags, GROWSDOWN)
		| AuConv_VM_MAP(flags, DENYWRITE)
		| AuConv_VM_MAP(flags, LOCKED);
}
#endif

static int aufs_mmap(struct file *file, struct vm_area_struct *vma)
{
	int err;
	aufs_bindex_t bstart;
	const unsigned char wlock
		= (file->f_mode & FMODE_WRITE) && (vma->vm_flags & VM_SHARED);
	struct dentry *dentry;
	struct super_block *sb;
	struct file *h_file;
	struct au_branch *br;
	struct au_pin pin;

	AuDbgVmRegion(file, vma);

	dentry = file->f_path.dentry;
	sb = dentry->d_sb;
	lockdep_off();
	si_read_lock(sb, AuLock_NOPLMW);
	err = au_reval_and_lock_fdi(file, au_reopen_nondir, /*wlock*/1);
	if (unlikely(err))
		goto out;

	if (wlock) {
		err = au_ready_to_write(file, -1, &pin);
		di_write_unlock(dentry);
		if (unlikely(err)) {
			fi_write_unlock(file);
			goto out;
		}
		au_unpin(&pin);
	} else
		di_write_unlock(dentry);

	bstart = au_fbstart(file);
	br = au_sbr(sb, bstart);
	h_file = au_hf_top(file);
	get_file(h_file);
	au_set_mmapped(file);
	fi_write_unlock(file);
	lockdep_on();

	au_vm_file_reset(vma, h_file);
	/*
	 * we cannot call security_mmap_file() here since it may acquire
	 * mmap_sem or i_mutex.
	 *
	 * err = security_mmap_file(h_file, au_prot_conv(vma->vm_flags),
	 *			 au_flag_conv(vma->vm_flags));
	 */
	if (!err)
		err = h_file->f_op->mmap(h_file, vma);
	if (unlikely(err))
		goto out_reset;

	au_vm_prfile_set(vma, file);
	/* update without lock, I don't think it a problem */
	fsstack_copy_attr_atime(file_inode(file), file_inode(h_file));
	goto out_fput; /* success */

out_reset:
	au_unset_mmapped(file);
	au_vm_file_reset(vma, file);
out_fput:
	fput(h_file);
	lockdep_off();
out:
	si_read_unlock(sb);
	lockdep_on();
	AuTraceErr(err);
	return err;
}

/* ---------------------------------------------------------------------- */

const struct file_operations aufs_file_fop = {
	.owner		= THIS_MODULE,

	.mmap		= aufs_mmap,
	.open		= aufs_open_nondir,
	.release	= aufs_release_nondir
};
