/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_DMA_NONCOHERENT_H
#define _LINUX_DMA_NONCOHERENT_H 1

#include <linux/dma-mapping.h>

#ifdef CONFIG_ARCH_HAS_DMA_COHERENCE_H
#include <asm/dma-coherence.h>
#elif defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_DEVICE) || \
	defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU) || \
	defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU_ALL)
static inline bool dev_is_dma_coherent(struct device *dev)
{
	return dev->dma_coherent;
}
#else
static inline bool dev_is_dma_coherent(struct device *dev)
{
	return true;
}
#endif /* CONFIG_ARCH_HAS_DMA_COHERENCE_H */

void *arch_dma_alloc(struct device *dev, size_t size, dma_addr_t *dma_handle,
		gfp_t gfp, unsigned long attrs);
void arch_dma_free(struct device *dev, size_t size, void *cpu_addr,
		dma_addr_t dma_addr, unsigned long attrs);
long arch_dma_coherent_to_pfn(struct device *dev, void *cpu_addr,
		dma_addr_t dma_addr);

#ifdef CONFIG_ARCH_HAS_DMA_MMAP_PGPROT
pgprot_t arch_dma_mmap_pgprot(struct device *dev, pgprot_t prot,
		unsigned long attrs);
#else
# define arch_dma_mmap_pgprot(dev, prot, attrs)	pgprot_noncached(prot)
#endif

#ifdef CONFIG_DMA_NONCOHERENT_CACHE_SYNC
void arch_dma_cache_sync(struct device *dev, void *vaddr, size_t size,
		enum dma_data_direction direction);
#else
#define arch_dma_cache_sync NULL
#endif /* CONFIG_DMA_NONCOHERENT_CACHE_SYNC */

#ifdef CONFIG_ARCH_HAS_SYNC_DMA_FOR_DEVICE
void arch_sync_dma_for_device(struct device *dev, phys_addr_t paddr,
		size_t size, enum dma_data_direction dir);
#else
static inline void arch_sync_dma_for_device(struct device *dev,
		phys_addr_t paddr, size_t size, enum dma_data_direction dir)
{
}
#endif /* ARCH_HAS_SYNC_DMA_FOR_DEVICE */

#ifdef CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU
void arch_sync_dma_for_cpu(struct device *dev, phys_addr_t paddr,
		size_t size, enum dma_data_direction dir);
#else
static inline void arch_sync_dma_for_cpu(struct device *dev,
		phys_addr_t paddr, size_t size, enum dma_data_direction dir)
{
}
#endif /* ARCH_HAS_SYNC_DMA_FOR_CPU */

#ifdef CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU_ALL
void arch_sync_dma_for_cpu_all(struct device *dev);
#else
static inline void arch_sync_dma_for_cpu_all(struct device *dev)
{
}
#endif /* CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU_ALL */

#endif /* _LINUX_DMA_NONCOHERENT_H */
