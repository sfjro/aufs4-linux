
# defined in ${srctree}/fs/configfs/mount.c
# tristate
ifdef CONFIG_CONFIGFS_FS
ccflags-y += -DCONFIGFS_MAGIC=0x62656570
endif
