
# cf. include/linux/kernel.h
# enable pr_debug
ccflags-y += -DDEBUG
# sparse requires the full pathname
ccflags-y += -include ${srctree}/include/uapi/linux/aufs_type.h

obj-$(CONFIG_AUFS_FS) += aufs.o
aufs-y := module.o sbinfo.o super.o \
	dcsub.o \
	cpup.o \
	dinfo.o \
	iinfo.o

# all are boolean
aufs-$(CONFIG_AUFS_DEBUG) += debug.o
