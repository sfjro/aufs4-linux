
# cf. include/linux/kernel.h
# enable pr_debug
ccflags-y += -DDEBUG
# sparse requires the full pathname
ccflags-y += -include ${srctree}/include/uapi/linux/aufs_type.h

obj-$(CONFIG_AUFS_FS) += aufs.o
aufs-y := module.o super.o \
	iinfo.o
