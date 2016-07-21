/*
 * Copyright (C) 2005-2016 Junjiro R. Okajima
 */

/*
 * module initialization and module-global
 */

#ifndef __AUFS_MODULE_H__
#define __AUFS_MODULE_H__

#ifdef __KERNEL__

#include <linux/slab.h>
#include "debug.h"

struct path;
struct seq_file;

/* module parameters */
extern int sysaufs_brs;
extern bool au_userns;

/* ---------------------------------------------------------------------- */

extern int au_dir_roflags;

void *au_kzrealloc(void *p, unsigned int nused, unsigned int new_sz, gfp_t gfp);
int au_seq_path(struct seq_file *seq, struct path *path);

#ifdef CONFIG_PROC_FS
/* procfs.c */
int __init au_procfs_init(void);
void au_procfs_fin(void);
#else
AuStubInt0(au_procfs_init, void);
AuStubVoid(au_procfs_fin, void);
#endif

/* ---------------------------------------------------------------------- */

/* kmem cache */
enum {
	AuCache_DINFO,
	AuCache_ICNTNR,
	AuCache_FINFO,
	AuCache_VDIR,
	AuCache_DEHSTR,
	AuCache_HNOTIFY, /* must be last */
	AuCache_Last
};

#define AuCacheFlags		(SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD)
#define AuCache(type)		KMEM_CACHE(type, AuCacheFlags)
#define AuCacheCtor(type, ctor)	\
	kmem_cache_create(#type, sizeof(struct type), \
			  __alignof__(struct type), AuCacheFlags, ctor)

extern struct kmem_cache *au_cachep[];

#define AuCacheFuncs(name, index) \
static inline struct au_##name *au_cache_alloc_##name(void) \
{ return kmem_cache_alloc(au_cachep[AuCache_##index], GFP_NOFS); } \
static inline void au_cache_free_##name(struct au_##name *p) \
{ kmem_cache_free(au_cachep[AuCache_##index], p); } \
static inline void au_cache_do_free_##name##_rcu(struct rcu_head *head) \
{ kmem_cache_free(au_cachep[AuCache_##index], head); } \
static inline void au_cache_delayed_free_##name(struct au_##name *p) \
{ call_rcu((void *)p, au_cache_do_free_##name##_rcu); }

AuCacheFuncs(dinfo, DINFO);
AuCacheFuncs(icntnr, ICNTNR);
AuCacheFuncs(finfo, FINFO);
AuCacheFuncs(vdir, VDIR);
AuCacheFuncs(vdir_dehstr, DEHSTR);
#ifdef CONFIG_AUFS_HNOTIFY
AuCacheFuncs(hnotify, HNOTIFY);
#endif

/* ---------------------------------------------------------------------- */

static inline void au_delayed_kfree(const void *p)
{
	AuDebugOn(ksize(p) < sizeof(struct rcu_head));
	__kfree_rcu((void *)p, /*offset*/0);
}

#endif /* __KERNEL__ */
#endif /* __AUFS_MODULE_H__ */
