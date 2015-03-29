
include ${srctree}/${src}/magic.mk

# cf. include/linux/kernel.h
# enable pr_debug
ccflags-y += -DDEBUG
# sparse requires the full pathname
ccflags-y += -include ${srctree}/include/uapi/linux/aufs_type.h

obj-$(CONFIG_AUFS_FS) += aufs.o
aufs-y := module.o sbinfo.o super.o branch.o xino.o sysaufs.o opts.o \
	wkq.o vfsub.o dcsub.o \
	cpup.o whout.o \
	dinfo.o \
	finfo.o \
	dir.o vdir.o \
	iinfo.o inode.o

# all are boolean
aufs-$(CONFIG_PROC_FS) += procfs.o plink.o
aufs-$(CONFIG_SYSFS) += sysfs.o
aufs-$(CONFIG_AUFS_DEBUG) += debug.o
