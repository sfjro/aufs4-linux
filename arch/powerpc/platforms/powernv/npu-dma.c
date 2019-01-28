/*
 * This file implements the DMA operations for NVLink devices. The NPU
 * devices all point to the same iommu table as the parent PCI device.
 *
 * Copyright Alistair Popple, IBM Corporation 2015.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 */

#include <linux/mmu_notifier.h>
#include <linux/mmu_context.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/memblock.h>
#include <linux/sizes.h>

#include <asm/debugfs.h>
#include <asm/powernv.h>
#include <asm/opal.h>

#include "pci.h"

/*
 * spinlock to protect initialisation of an npu_context for a particular
 * mm_struct.
 */
static DEFINE_SPINLOCK(npu_context_lock);

/*
 * Other types of TCE cache invalidation are not functional in the
 * hardware.
 */
static struct pci_dev *get_pci_dev(struct device_node *dn)
{
	struct pci_dn *pdn = PCI_DN(dn);

	return pci_get_domain_bus_and_slot(pci_domain_nr(pdn->phb->bus),
					   pdn->busno, pdn->devfn);
}

/* Given a NPU device get the associated PCI device. */
struct pci_dev *pnv_pci_get_gpu_dev(struct pci_dev *npdev)
{
	struct device_node *dn;
	struct pci_dev *gpdev;

	if (WARN_ON(!npdev))
		return NULL;

	if (WARN_ON(!npdev->dev.of_node))
		return NULL;

	/* Get assoicated PCI device */
	dn = of_parse_phandle(npdev->dev.of_node, "ibm,gpu", 0);
	if (!dn)
		return NULL;

	gpdev = get_pci_dev(dn);
	of_node_put(dn);

	return gpdev;
}
EXPORT_SYMBOL(pnv_pci_get_gpu_dev);

/* Given the real PCI device get a linked NPU device. */
struct pci_dev *pnv_pci_get_npu_dev(struct pci_dev *gpdev, int index)
{
	struct device_node *dn;
	struct pci_dev *npdev;

	if (WARN_ON(!gpdev))
		return NULL;

	/* Not all PCI devices have device-tree nodes */
	if (!gpdev->dev.of_node)
		return NULL;

	/* Get assoicated PCI device */
	dn = of_parse_phandle(gpdev->dev.of_node, "ibm,npu", index);
	if (!dn)
		return NULL;

	npdev = get_pci_dev(dn);
	of_node_put(dn);

	return npdev;
}
EXPORT_SYMBOL(pnv_pci_get_npu_dev);

/*
 * Returns the PE assoicated with the PCI device of the given
 * NPU. Returns the linked pci device if pci_dev != NULL.
 */
static struct pnv_ioda_pe *get_gpu_pci_dev_and_pe(struct pnv_ioda_pe *npe,
						  struct pci_dev **gpdev)
{
	struct pnv_phb *phb;
	struct pci_controller *hose;
	struct pci_dev *pdev;
	struct pnv_ioda_pe *pe;
	struct pci_dn *pdn;

	pdev = pnv_pci_get_gpu_dev(npe->pdev);
	if (!pdev)
		return NULL;

	pdn = pci_get_pdn(pdev);
	if (WARN_ON(!pdn || pdn->pe_number == IODA_INVALID_PE))
		return NULL;

	hose = pci_bus_to_host(pdev->bus);
	phb = hose->private_data;
	pe = &phb->ioda.pe_array[pdn->pe_number];

	if (gpdev)
		*gpdev = pdev;

	return pe;
}

static long pnv_npu_unset_window(struct iommu_table_group *table_group,
		int num);

static long pnv_npu_set_window(struct iommu_table_group *table_group, int num,
		struct iommu_table *tbl)
{
	struct pnv_ioda_pe *npe = container_of(table_group, struct pnv_ioda_pe,
			table_group);
	struct pnv_phb *phb = npe->phb;
	int64_t rc;
	const unsigned long size = tbl->it_indirect_levels ?
		tbl->it_level_size : tbl->it_size;
	const __u64 start_addr = tbl->it_offset << tbl->it_page_shift;
	const __u64 win_size = tbl->it_size << tbl->it_page_shift;
	int num2 = (num == 0) ? 1 : 0;

	/* NPU has just one TVE so if there is another table, remove it first */
	if (npe->table_group.tables[num2])
		pnv_npu_unset_window(&npe->table_group, num2);

	pe_info(npe, "Setting up window %llx..%llx pg=%lx\n",
			start_addr, start_addr + win_size - 1,
			IOMMU_PAGE_SIZE(tbl));

	rc = opal_pci_map_pe_dma_window(phb->opal_id,
			npe->pe_number,
			npe->pe_number,
			tbl->it_indirect_levels + 1,
			__pa(tbl->it_base),
			size << 3,
			IOMMU_PAGE_SIZE(tbl));
	if (rc) {
		pe_err(npe, "Failed to configure TCE table, err %lld\n", rc);
		return rc;
	}
	pnv_pci_ioda2_tce_invalidate_entire(phb, false);

	/* Add the table to the list so its TCE cache will get invalidated */
	pnv_pci_link_table_and_group(phb->hose->node, num,
			tbl, &npe->table_group);

	return 0;
}

static long pnv_npu_unset_window(struct iommu_table_group *table_group, int num)
{
	struct pnv_ioda_pe *npe = container_of(table_group, struct pnv_ioda_pe,
			table_group);
	struct pnv_phb *phb = npe->phb;
	int64_t rc;

	if (!npe->table_group.tables[num])
		return 0;

	pe_info(npe, "Removing DMA window\n");

	rc = opal_pci_map_pe_dma_window(phb->opal_id, npe->pe_number,
			npe->pe_number,
			0/* levels */, 0/* table address */,
			0/* table size */, 0/* page size */);
	if (rc) {
		pe_err(npe, "Unmapping failed, ret = %lld\n", rc);
		return rc;
	}
	pnv_pci_ioda2_tce_invalidate_entire(phb, false);

	pnv_pci_unlink_table_and_group(npe->table_group.tables[num],
			&npe->table_group);

	return 0;
}

/*
 * Enables 32 bit DMA on NPU.
 */
static void pnv_npu_dma_set_32(struct pnv_ioda_pe *npe)
{
	struct pci_dev *gpdev;
	struct pnv_ioda_pe *gpe;
	int64_t rc;

	/*
	 * Find the assoicated PCI devices and get the dma window
	 * information from there.
	 */
	if (!npe->pdev || !(npe->flags & PNV_IODA_PE_DEV))
		return;

	gpe = get_gpu_pci_dev_and_pe(npe, &gpdev);
	if (!gpe)
		return;

	rc = pnv_npu_set_window(&npe->table_group, 0,
			gpe->table_group.tables[0]);

	/*
	 * NVLink devices use the same TCE table configuration as
	 * their parent device so drivers shouldn't be doing DMA
	 * operations directly on these devices.
	 */
	set_dma_ops(&npe->pdev->dev, NULL);
}

/*
 * Enables bypass mode on the NPU. The NPU only supports one
 * window per link, so bypass needs to be explicitly enabled or
 * disabled. Unlike for a PHB3 bypass and non-bypass modes can't be
 * active at the same time.
 */
static int pnv_npu_dma_set_bypass(struct pnv_ioda_pe *npe)
{
	struct pnv_phb *phb = npe->phb;
	int64_t rc = 0;
	phys_addr_t top = memblock_end_of_DRAM();

	if (phb->type != PNV_PHB_NPU_NVLINK || !npe->pdev)
		return -EINVAL;

	rc = pnv_npu_unset_window(&npe->table_group, 0);
	if (rc != OPAL_SUCCESS)
		return rc;

	/* Enable the bypass window */

	top = roundup_pow_of_two(top);
	dev_info(&npe->pdev->dev, "Enabling bypass for PE %x\n",
			npe->pe_number);
	rc = opal_pci_map_pe_dma_window_real(phb->opal_id,
			npe->pe_number, npe->pe_number,
			0 /* bypass base */, top);

	if (rc == OPAL_SUCCESS)
		pnv_pci_ioda2_tce_invalidate_entire(phb, false);

	return rc;
}

void pnv_npu_try_dma_set_bypass(struct pci_dev *gpdev, bool bypass)
{
	int i;
	struct pnv_phb *phb;
	struct pci_dn *pdn;
	struct pnv_ioda_pe *npe;
	struct pci_dev *npdev;

	for (i = 0; ; ++i) {
		npdev = pnv_pci_get_npu_dev(gpdev, i);

		if (!npdev)
			break;

		pdn = pci_get_pdn(npdev);
		if (WARN_ON(!pdn || pdn->pe_number == IODA_INVALID_PE))
			return;

		phb = pci_bus_to_host(npdev->bus)->private_data;

		/* We only do bypass if it's enabled on the linked device */
		npe = &phb->ioda.pe_array[pdn->pe_number];

		if (bypass) {
			dev_info(&npdev->dev,
					"Using 64-bit DMA iommu bypass\n");
			pnv_npu_dma_set_bypass(npe);
		} else {
			dev_info(&npdev->dev, "Using 32-bit DMA via iommu\n");
			pnv_npu_dma_set_32(npe);
		}
	}
}

#ifdef CONFIG_IOMMU_API
/* Switch ownership from platform code to external user (e.g. VFIO) */
static void pnv_npu_take_ownership(struct iommu_table_group *table_group)
{
	struct pnv_ioda_pe *npe = container_of(table_group, struct pnv_ioda_pe,
			table_group);
	struct pnv_phb *phb = npe->phb;
	int64_t rc;
	struct pci_dev *gpdev = NULL;

	/*
	 * Note: NPU has just a single TVE in the hardware which means that
	 * while used by the kernel, it can have either 32bit window or
	 * DMA bypass but never both. So we deconfigure 32bit window only
	 * if it was enabled at the moment of ownership change.
	 */
	if (npe->table_group.tables[0]) {
		pnv_npu_unset_window(&npe->table_group, 0);
		return;
	}

	/* Disable bypass */
	rc = opal_pci_map_pe_dma_window_real(phb->opal_id,
			npe->pe_number, npe->pe_number,
			0 /* bypass base */, 0);
	if (rc) {
		pe_err(npe, "Failed to disable bypass, err %lld\n", rc);
		return;
	}
	pnv_pci_ioda2_tce_invalidate_entire(npe->phb, false);

	get_gpu_pci_dev_and_pe(npe, &gpdev);
	if (gpdev)
		pnv_npu2_unmap_lpar_dev(gpdev);
}

static void pnv_npu_release_ownership(struct iommu_table_group *table_group)
{
	struct pnv_ioda_pe *npe = container_of(table_group, struct pnv_ioda_pe,
			table_group);
	struct pci_dev *gpdev = NULL;

	get_gpu_pci_dev_and_pe(npe, &gpdev);
	if (gpdev)
		pnv_npu2_map_lpar_dev(gpdev, 0, MSR_DR | MSR_PR | MSR_HV);
}

static struct iommu_table_group_ops pnv_pci_npu_ops = {
	.set_window = pnv_npu_set_window,
	.unset_window = pnv_npu_unset_window,
	.take_ownership = pnv_npu_take_ownership,
	.release_ownership = pnv_npu_release_ownership,
};
#endif /* !CONFIG_IOMMU_API */

/*
 * NPU2 ATS
 */
/* Maximum possible number of ATSD MMIO registers per NPU */
#define NV_NMMU_ATSD_REGS 8
#define NV_NPU_MAX_PE_NUM	16

/*
 * A compound NPU IOMMU group which might consist of 1 GPU + 2xNPUs (POWER8) or
 * up to 3 x (GPU + 2xNPUs) (POWER9).
 */
struct npu_comp {
	struct iommu_table_group table_group;
	int pe_num;
	struct pnv_ioda_pe *pe[NV_NPU_MAX_PE_NUM];
};

/* An NPU descriptor, valid for POWER9 only */
struct npu {
	int index;
	__be64 *mmio_atsd_regs[NV_NMMU_ATSD_REGS];
	unsigned int mmio_atsd_count;

	/* Bitmask for MMIO register usage */
	unsigned long mmio_atsd_usage;

	/* Do we need to explicitly flush the nest mmu? */
	bool nmmu_flush;

	struct npu_comp npucomp;
};

#ifdef CONFIG_IOMMU_API
static long pnv_npu_peers_create_table_userspace(
		struct iommu_table_group *table_group,
		int num, __u32 page_shift, __u64 window_size, __u32 levels,
		struct iommu_table **ptbl)
{
	struct npu_comp *npucomp = container_of(table_group, struct npu_comp,
			table_group);

	if (!npucomp->pe_num || !npucomp->pe[0] ||
			!npucomp->pe[0]->table_group.ops ||
			!npucomp->pe[0]->table_group.ops->create_table)
		return -EFAULT;

	return npucomp->pe[0]->table_group.ops->create_table(
			&npucomp->pe[0]->table_group, num, page_shift,
			window_size, levels, ptbl);
}

static long pnv_npu_peers_set_window(struct iommu_table_group *table_group,
		int num, struct iommu_table *tbl)
{
	int i, j;
	long ret = 0;
	struct npu_comp *npucomp = container_of(table_group, struct npu_comp,
			table_group);

	for (i = 0; i < npucomp->pe_num; ++i) {
		struct pnv_ioda_pe *pe = npucomp->pe[i];

		if (!pe->table_group.ops->set_window)
			continue;

		ret = pe->table_group.ops->set_window(&pe->table_group,
				num, tbl);
		if (ret)
			break;
	}

	if (ret) {
		for (j = 0; j < i; ++j) {
			struct pnv_ioda_pe *pe = npucomp->pe[j];

			if (!pe->table_group.ops->unset_window)
				continue;

			ret = pe->table_group.ops->unset_window(
					&pe->table_group, num);
			if (ret)
				break;
		}
	} else {
		table_group->tables[num] = iommu_tce_table_get(tbl);
	}

	return ret;
}

static long pnv_npu_peers_unset_window(struct iommu_table_group *table_group,
		int num)
{
	int i, j;
	long ret = 0;
	struct npu_comp *npucomp = container_of(table_group, struct npu_comp,
			table_group);

	for (i = 0; i < npucomp->pe_num; ++i) {
		struct pnv_ioda_pe *pe = npucomp->pe[i];

		WARN_ON(npucomp->table_group.tables[num] !=
				table_group->tables[num]);
		if (!npucomp->table_group.tables[num])
			continue;

		if (!pe->table_group.ops->unset_window)
			continue;

		ret = pe->table_group.ops->unset_window(&pe->table_group, num);
		if (ret)
			break;
	}

	if (ret) {
		for (j = 0; j < i; ++j) {
			struct pnv_ioda_pe *pe = npucomp->pe[j];

			if (!npucomp->table_group.tables[num])
				continue;

			if (!pe->table_group.ops->set_window)
				continue;

			ret = pe->table_group.ops->set_window(&pe->table_group,
					num, table_group->tables[num]);
			if (ret)
				break;
		}
	} else if (table_group->tables[num]) {
		iommu_tce_table_put(table_group->tables[num]);
		table_group->tables[num] = NULL;
	}

	return ret;
}

static void pnv_npu_peers_take_ownership(struct iommu_table_group *table_group)
{
	int i;
	struct npu_comp *npucomp = container_of(table_group, struct npu_comp,
			table_group);

	for (i = 0; i < npucomp->pe_num; ++i) {
		struct pnv_ioda_pe *pe = npucomp->pe[i];

		if (!pe->table_group.ops->take_ownership)
			continue;
		pe->table_group.ops->take_ownership(&pe->table_group);
	}
}

static void pnv_npu_peers_release_ownership(
		struct iommu_table_group *table_group)
{
	int i;
	struct npu_comp *npucomp = container_of(table_group, struct npu_comp,
			table_group);

	for (i = 0; i < npucomp->pe_num; ++i) {
		struct pnv_ioda_pe *pe = npucomp->pe[i];

		if (!pe->table_group.ops->release_ownership)
			continue;
		pe->table_group.ops->release_ownership(&pe->table_group);
	}
}

static struct iommu_table_group_ops pnv_npu_peers_ops = {
	.get_table_size = pnv_pci_ioda2_get_table_size,
	.create_table = pnv_npu_peers_create_table_userspace,
	.set_window = pnv_npu_peers_set_window,
	.unset_window = pnv_npu_peers_unset_window,
	.take_ownership = pnv_npu_peers_take_ownership,
	.release_ownership = pnv_npu_peers_release_ownership,
};

static void pnv_comp_attach_table_group(struct npu_comp *npucomp,
		struct pnv_ioda_pe *pe)
{
	if (WARN_ON(npucomp->pe_num == NV_NPU_MAX_PE_NUM))
		return;

	npucomp->pe[npucomp->pe_num] = pe;
	++npucomp->pe_num;
}

struct iommu_table_group *pnv_try_setup_npu_table_group(struct pnv_ioda_pe *pe)
{
	struct iommu_table_group *table_group;
	struct npu_comp *npucomp;
	struct pci_dev *gpdev = NULL;
	struct pci_controller *hose;
	struct pci_dev *npdev = NULL;

	list_for_each_entry(gpdev, &pe->pbus->devices, bus_list) {
		npdev = pnv_pci_get_npu_dev(gpdev, 0);
		if (npdev)
			break;
	}

	if (!npdev)
		/* It is not an NPU attached device, skip */
		return NULL;

	hose = pci_bus_to_host(npdev->bus);

	if (hose->npu) {
		table_group = &hose->npu->npucomp.table_group;

		if (!table_group->group) {
			table_group->ops = &pnv_npu_peers_ops;
			iommu_register_group(table_group,
					hose->global_number,
					pe->pe_number);
		}
	} else {
		/* Create a group for 1 GPU and attached NPUs for POWER8 */
		pe->npucomp = kzalloc(sizeof(*pe->npucomp), GFP_KERNEL);
		table_group = &pe->npucomp->table_group;
		table_group->ops = &pnv_npu_peers_ops;
		iommu_register_group(table_group, hose->global_number,
				pe->pe_number);
	}

	/* Steal capabilities from a GPU PE */
	table_group->max_dynamic_windows_supported =
		pe->table_group.max_dynamic_windows_supported;
	table_group->tce32_start = pe->table_group.tce32_start;
	table_group->tce32_size = pe->table_group.tce32_size;
	table_group->max_levels = pe->table_group.max_levels;
	if (!table_group->pgsizes)
		table_group->pgsizes = pe->table_group.pgsizes;

	npucomp = container_of(table_group, struct npu_comp, table_group);
	pnv_comp_attach_table_group(npucomp, pe);

	return table_group;
}

struct iommu_table_group *pnv_npu_compound_attach(struct pnv_ioda_pe *pe)
{
	struct iommu_table_group *table_group;
	struct npu_comp *npucomp;
	struct pci_dev *gpdev = NULL;
	struct pci_dev *npdev;
	struct pnv_ioda_pe *gpe = get_gpu_pci_dev_and_pe(pe, &gpdev);

	WARN_ON(!(pe->flags & PNV_IODA_PE_DEV));
	if (!gpe)
		return NULL;

	/*
	 * IODA2 bridges get this set up from pci_controller_ops::setup_bridge
	 * but NPU bridges do not have this hook defined so we do it here.
	 * We do not setup other table group parameters as they won't be used
	 * anyway - NVLink bridges are subordinate PEs.
	 */
	pe->table_group.ops = &pnv_pci_npu_ops;

	table_group = iommu_group_get_iommudata(
			iommu_group_get(&gpdev->dev));

	/*
	 * On P9 NPU PHB and PCI PHB support different page sizes,
	 * keep only matching. We expect here that NVLink bridge PE pgsizes is
	 * initialized by the caller.
	 */
	table_group->pgsizes &= pe->table_group.pgsizes;
	npucomp = container_of(table_group, struct npu_comp, table_group);
	pnv_comp_attach_table_group(npucomp, pe);

	list_for_each_entry(npdev, &pe->phb->hose->bus->devices, bus_list) {
		struct pci_dev *gpdevtmp = pnv_pci_get_gpu_dev(npdev);

		if (gpdevtmp != gpdev)
			continue;

		iommu_add_device(table_group, &npdev->dev);
	}

	return table_group;
}
#endif /* CONFIG_IOMMU_API */

/* Maximum number of nvlinks per npu */
#define NV_MAX_LINKS 6

/* Maximum index of npu2 hosts in the system. Always < NV_MAX_NPUS */
static int max_npu2_index;

struct npu_context {
	struct mm_struct *mm;
	struct pci_dev *npdev[NV_MAX_NPUS][NV_MAX_LINKS];
	struct mmu_notifier mn;
	struct kref kref;
	bool nmmu_flush;

	/* Callback to stop translation requests on a given GPU */
	void (*release_cb)(struct npu_context *context, void *priv);

	/*
	 * Private pointer passed to the above callback for usage by
	 * device drivers.
	 */
	void *priv;
};

struct mmio_atsd_reg {
	struct npu *npu;
	int reg;
};

/*
 * Find a free MMIO ATSD register and mark it in use. Return -ENOSPC
 * if none are available.
 */
static int get_mmio_atsd_reg(struct npu *npu)
{
	int i;

	for (i = 0; i < npu->mmio_atsd_count; i++) {
		if (!test_bit(i, &npu->mmio_atsd_usage))
			if (!test_and_set_bit_lock(i, &npu->mmio_atsd_usage))
				return i;
	}

	return -ENOSPC;
}

static void put_mmio_atsd_reg(struct npu *npu, int reg)
{
	clear_bit_unlock(reg, &npu->mmio_atsd_usage);
}

/* MMIO ATSD register offsets */
#define XTS_ATSD_LAUNCH 0
#define XTS_ATSD_AVA    1
#define XTS_ATSD_STAT   2

static unsigned long get_atsd_launch_val(unsigned long pid, unsigned long psize)
{
	unsigned long launch = 0;

	if (psize == MMU_PAGE_COUNT) {
		/* IS set to invalidate entire matching PID */
		launch |= PPC_BIT(12);
	} else {
		/* AP set to invalidate region of psize */
		launch |= (u64)mmu_get_ap(psize) << PPC_BITLSHIFT(17);
	}

	/* PRS set to process-scoped */
	launch |= PPC_BIT(13);

	/* PID */
	launch |= pid << PPC_BITLSHIFT(38);

	/* Leave "No flush" (bit 39) 0 so every ATSD performs a flush */

	return launch;
}

static void mmio_atsd_regs_write(struct mmio_atsd_reg
			mmio_atsd_reg[NV_MAX_NPUS], unsigned long offset,
			unsigned long val)
{
	struct npu *npu;
	int i, reg;

	for (i = 0; i <= max_npu2_index; i++) {
		reg = mmio_atsd_reg[i].reg;
		if (reg < 0)
			continue;

		npu = mmio_atsd_reg[i].npu;
		__raw_writeq_be(val, npu->mmio_atsd_regs[reg] + offset);
	}
}

static void mmio_invalidate_pid(struct mmio_atsd_reg mmio_atsd_reg[NV_MAX_NPUS],
				unsigned long pid)
{
	unsigned long launch = get_atsd_launch_val(pid, MMU_PAGE_COUNT);

	/* Invalidating the entire process doesn't use a va */
	mmio_atsd_regs_write(mmio_atsd_reg, XTS_ATSD_LAUNCH, launch);
}

static void mmio_invalidate_range(struct mmio_atsd_reg
			mmio_atsd_reg[NV_MAX_NPUS], unsigned long pid,
			unsigned long start, unsigned long psize)
{
	unsigned long launch = get_atsd_launch_val(pid, psize);

	/* Write all VAs first */
	mmio_atsd_regs_write(mmio_atsd_reg, XTS_ATSD_AVA, start);

	/* Issue one barrier for all address writes */
	eieio();

	/* Launch */
	mmio_atsd_regs_write(mmio_atsd_reg, XTS_ATSD_LAUNCH, launch);
}

#define mn_to_npu_context(x) container_of(x, struct npu_context, mn)

static void mmio_invalidate_wait(
	struct mmio_atsd_reg mmio_atsd_reg[NV_MAX_NPUS])
{
	struct npu *npu;
	int i, reg;

	/* Wait for all invalidations to complete */
	for (i = 0; i <= max_npu2_index; i++) {
		if (mmio_atsd_reg[i].reg < 0)
			continue;

		/* Wait for completion */
		npu = mmio_atsd_reg[i].npu;
		reg = mmio_atsd_reg[i].reg;
		while (__raw_readq(npu->mmio_atsd_regs[reg] + XTS_ATSD_STAT))
			cpu_relax();
	}
}

/*
 * Acquires all the address translation shootdown (ATSD) registers required to
 * launch an ATSD on all links this npu_context is active on.
 */
static void acquire_atsd_reg(struct npu_context *npu_context,
			struct mmio_atsd_reg mmio_atsd_reg[NV_MAX_NPUS])
{
	int i, j;
	struct npu *npu;
	struct pci_dev *npdev;

	for (i = 0; i <= max_npu2_index; i++) {
		mmio_atsd_reg[i].reg = -1;
		for (j = 0; j < NV_MAX_LINKS; j++) {
			/*
			 * There are no ordering requirements with respect to
			 * the setup of struct npu_context, but to ensure
			 * consistent behaviour we need to ensure npdev[][] is
			 * only read once.
			 */
			npdev = READ_ONCE(npu_context->npdev[i][j]);
			if (!npdev)
				continue;

			npu = pci_bus_to_host(npdev->bus)->npu;
			if (!npu)
				continue;

			mmio_atsd_reg[i].npu = npu;
			mmio_atsd_reg[i].reg = get_mmio_atsd_reg(npu);
			while (mmio_atsd_reg[i].reg < 0) {
				mmio_atsd_reg[i].reg = get_mmio_atsd_reg(npu);
				cpu_relax();
			}
			break;
		}
	}
}

/*
 * Release previously acquired ATSD registers. To avoid deadlocks the registers
 * must be released in the same order they were acquired above in
 * acquire_atsd_reg.
 */
static void release_atsd_reg(struct mmio_atsd_reg mmio_atsd_reg[NV_MAX_NPUS])
{
	int i;

	for (i = 0; i <= max_npu2_index; i++) {
		/*
		 * We can't rely on npu_context->npdev[][] being the same here
		 * as when acquire_atsd_reg() was called, hence we use the
		 * values stored in mmio_atsd_reg during the acquire phase
		 * rather than re-reading npdev[][].
		 */
		if (mmio_atsd_reg[i].reg < 0)
			continue;

		put_mmio_atsd_reg(mmio_atsd_reg[i].npu, mmio_atsd_reg[i].reg);
	}
}

/*
 * Invalidate a virtual address range
 */
static void mmio_invalidate(struct npu_context *npu_context,
			unsigned long start, unsigned long size)
{
	struct mmio_atsd_reg mmio_atsd_reg[NV_MAX_NPUS];
	unsigned long pid = npu_context->mm->context.id;
	unsigned long atsd_start = 0;
	unsigned long end = start + size - 1;
	int atsd_psize = MMU_PAGE_COUNT;

	/*
	 * Convert the input range into one of the supported sizes. If the range
	 * doesn't fit, use the next larger supported size. Invalidation latency
	 * is high, so over-invalidation is preferred to issuing multiple
	 * invalidates.
	 *
	 * A 4K page size isn't supported by NPU/GPU ATS, so that case is
	 * ignored.
	 */
	if (size == SZ_64K) {
		atsd_start = start;
		atsd_psize = MMU_PAGE_64K;
	} else if (ALIGN_DOWN(start, SZ_2M) == ALIGN_DOWN(end, SZ_2M)) {
		atsd_start = ALIGN_DOWN(start, SZ_2M);
		atsd_psize = MMU_PAGE_2M;
	} else if (ALIGN_DOWN(start, SZ_1G) == ALIGN_DOWN(end, SZ_1G)) {
		atsd_start = ALIGN_DOWN(start, SZ_1G);
		atsd_psize = MMU_PAGE_1G;
	}

	if (npu_context->nmmu_flush)
		/*
		 * Unfortunately the nest mmu does not support flushing specific
		 * addresses so we have to flush the whole mm once before
		 * shooting down the GPU translation.
		 */
		flush_all_mm(npu_context->mm);

	/*
	 * Loop over all the NPUs this process is active on and launch
	 * an invalidate.
	 */
	acquire_atsd_reg(npu_context, mmio_atsd_reg);

	if (atsd_psize == MMU_PAGE_COUNT)
		mmio_invalidate_pid(mmio_atsd_reg, pid);
	else
		mmio_invalidate_range(mmio_atsd_reg, pid, atsd_start,
					atsd_psize);

	mmio_invalidate_wait(mmio_atsd_reg);

	/*
	 * The GPU requires two flush ATSDs to ensure all entries have been
	 * flushed. We use PID 0 as it will never be used for a process on the
	 * GPU.
	 */
	mmio_invalidate_pid(mmio_atsd_reg, 0);
	mmio_invalidate_wait(mmio_atsd_reg);
	mmio_invalidate_pid(mmio_atsd_reg, 0);
	mmio_invalidate_wait(mmio_atsd_reg);

	release_atsd_reg(mmio_atsd_reg);
}

static void pnv_npu2_mn_release(struct mmu_notifier *mn,
				struct mm_struct *mm)
{
	struct npu_context *npu_context = mn_to_npu_context(mn);

	/* Call into device driver to stop requests to the NMMU */
	if (npu_context->release_cb)
		npu_context->release_cb(npu_context, npu_context->priv);

	/*
	 * There should be no more translation requests for this PID, but we
	 * need to ensure any entries for it are removed from the TLB.
	 */
	mmio_invalidate(npu_context, 0, ~0UL);
}

static void pnv_npu2_mn_change_pte(struct mmu_notifier *mn,
				struct mm_struct *mm,
				unsigned long address,
				pte_t pte)
{
	struct npu_context *npu_context = mn_to_npu_context(mn);
	mmio_invalidate(npu_context, address, PAGE_SIZE);
}

static void pnv_npu2_mn_invalidate_range(struct mmu_notifier *mn,
					struct mm_struct *mm,
					unsigned long start, unsigned long end)
{
	struct npu_context *npu_context = mn_to_npu_context(mn);
	mmio_invalidate(npu_context, start, end - start);
}

static const struct mmu_notifier_ops nv_nmmu_notifier_ops = {
	.release = pnv_npu2_mn_release,
	.change_pte = pnv_npu2_mn_change_pte,
	.invalidate_range = pnv_npu2_mn_invalidate_range,
};

/*
 * Call into OPAL to setup the nmmu context for the current task in
 * the NPU. This must be called to setup the context tables before the
 * GPU issues ATRs. pdev should be a pointed to PCIe GPU device.
 *
 * A release callback should be registered to allow a device driver to
 * be notified that it should not launch any new translation requests
 * as the final TLB invalidate is about to occur.
 *
 * Returns an error if there no contexts are currently available or a
 * npu_context which should be passed to pnv_npu2_handle_fault().
 *
 * mmap_sem must be held in write mode and must not be called from interrupt
 * context.
 */
struct npu_context *pnv_npu2_init_context(struct pci_dev *gpdev,
			unsigned long flags,
			void (*cb)(struct npu_context *, void *),
			void *priv)
{
	int rc;
	u32 nvlink_index;
	struct device_node *nvlink_dn;
	struct mm_struct *mm = current->mm;
	struct npu *npu;
	struct npu_context *npu_context;
	struct pci_controller *hose;

	/*
	 * At present we don't support GPUs connected to multiple NPUs and I'm
	 * not sure the hardware does either.
	 */
	struct pci_dev *npdev = pnv_pci_get_npu_dev(gpdev, 0);

	if (!npdev)
		/* No nvlink associated with this GPU device */
		return ERR_PTR(-ENODEV);

	/* We only support DR/PR/HV in pnv_npu2_map_lpar_dev() */
	if (flags & ~(MSR_DR | MSR_PR | MSR_HV))
		return ERR_PTR(-EINVAL);

	nvlink_dn = of_parse_phandle(npdev->dev.of_node, "ibm,nvlink", 0);
	if (WARN_ON(of_property_read_u32(nvlink_dn, "ibm,npu-link-index",
							&nvlink_index)))
		return ERR_PTR(-ENODEV);

	if (!mm || mm->context.id == 0) {
		/*
		 * Kernel thread contexts are not supported and context id 0 is
		 * reserved on the GPU.
		 */
		return ERR_PTR(-EINVAL);
	}

	hose = pci_bus_to_host(npdev->bus);
	npu = hose->npu;
	if (!npu)
		return ERR_PTR(-ENODEV);

	/*
	 * We store the npu pci device so we can more easily get at the
	 * associated npus.
	 */
	spin_lock(&npu_context_lock);
	npu_context = mm->context.npu_context;
	if (npu_context) {
		if (npu_context->release_cb != cb ||
			npu_context->priv != priv) {
			spin_unlock(&npu_context_lock);
			return ERR_PTR(-EINVAL);
		}

		WARN_ON(!kref_get_unless_zero(&npu_context->kref));
	}
	spin_unlock(&npu_context_lock);

	if (!npu_context) {
		/*
		 * We can set up these fields without holding the
		 * npu_context_lock as the npu_context hasn't been returned to
		 * the caller meaning it can't be destroyed. Parallel allocation
		 * is protected against by mmap_sem.
		 */
		rc = -ENOMEM;
		npu_context = kzalloc(sizeof(struct npu_context), GFP_KERNEL);
		if (npu_context) {
			kref_init(&npu_context->kref);
			npu_context->mm = mm;
			npu_context->mn.ops = &nv_nmmu_notifier_ops;
			rc = __mmu_notifier_register(&npu_context->mn, mm);
		}

		if (rc) {
			kfree(npu_context);
			return ERR_PTR(rc);
		}

		mm->context.npu_context = npu_context;
	}

	npu_context->release_cb = cb;
	npu_context->priv = priv;

	/*
	 * npdev is a pci_dev pointer setup by the PCI code. We assign it to
	 * npdev[][] to indicate to the mmu notifiers that an invalidation
	 * should also be sent over this nvlink. The notifiers don't use any
	 * other fields in npu_context, so we just need to ensure that when they
	 * deference npu_context->npdev[][] it is either a valid pointer or
	 * NULL.
	 */
	WRITE_ONCE(npu_context->npdev[npu->index][nvlink_index], npdev);

	if (!npu->nmmu_flush) {
		/*
		 * If we're not explicitly flushing ourselves we need to mark
		 * the thread for global flushes
		 */
		npu_context->nmmu_flush = false;
		mm_context_add_copro(mm);
	} else
		npu_context->nmmu_flush = true;

	return npu_context;
}
EXPORT_SYMBOL(pnv_npu2_init_context);

static void pnv_npu2_release_context(struct kref *kref)
{
	struct npu_context *npu_context =
		container_of(kref, struct npu_context, kref);

	if (!npu_context->nmmu_flush)
		mm_context_remove_copro(npu_context->mm);

	npu_context->mm->context.npu_context = NULL;
}

/*
 * Destroy a context on the given GPU. May free the npu_context if it is no
 * longer active on any GPUs. Must not be called from interrupt context.
 */
void pnv_npu2_destroy_context(struct npu_context *npu_context,
			struct pci_dev *gpdev)
{
	int removed;
	struct npu *npu;
	struct pci_dev *npdev = pnv_pci_get_npu_dev(gpdev, 0);
	struct device_node *nvlink_dn;
	u32 nvlink_index;
	struct pci_controller *hose;

	if (WARN_ON(!npdev))
		return;

	hose = pci_bus_to_host(npdev->bus);
	npu = hose->npu;
	if (!npu)
		return;
	nvlink_dn = of_parse_phandle(npdev->dev.of_node, "ibm,nvlink", 0);
	if (WARN_ON(of_property_read_u32(nvlink_dn, "ibm,npu-link-index",
							&nvlink_index)))
		return;
	WRITE_ONCE(npu_context->npdev[npu->index][nvlink_index], NULL);
	spin_lock(&npu_context_lock);
	removed = kref_put(&npu_context->kref, pnv_npu2_release_context);
	spin_unlock(&npu_context_lock);

	/*
	 * We need to do this outside of pnv_npu2_release_context so that it is
	 * outside the spinlock as mmu_notifier_destroy uses SRCU.
	 */
	if (removed) {
		mmu_notifier_unregister(&npu_context->mn,
					npu_context->mm);

		kfree(npu_context);
	}

}
EXPORT_SYMBOL(pnv_npu2_destroy_context);

/*
 * Assumes mmap_sem is held for the contexts associated mm.
 */
int pnv_npu2_handle_fault(struct npu_context *context, uintptr_t *ea,
			unsigned long *flags, unsigned long *status, int count)
{
	u64 rc = 0, result = 0;
	int i, is_write;
	struct page *page[1];
	const char __user *u;
	char c;

	/* mmap_sem should be held so the struct_mm must be present */
	struct mm_struct *mm = context->mm;

	WARN_ON(!rwsem_is_locked(&mm->mmap_sem));

	for (i = 0; i < count; i++) {
		is_write = flags[i] & NPU2_WRITE;
		rc = get_user_pages_remote(NULL, mm, ea[i], 1,
					is_write ? FOLL_WRITE : 0,
					page, NULL, NULL);

		if (rc != 1) {
			status[i] = rc;
			result = -EFAULT;
			continue;
		}

		/* Make sure partition scoped tree gets a pte */
		u = page_address(page[0]);
		if (__get_user(c, u))
			result = -EFAULT;

		status[i] = 0;
		put_page(page[0]);
	}

	return result;
}
EXPORT_SYMBOL(pnv_npu2_handle_fault);

int pnv_npu2_init(struct pci_controller *hose)
{
	unsigned int i;
	u64 mmio_atsd;
	static int npu_index;
	struct npu *npu;
	int ret;

	npu = kzalloc(sizeof(*npu), GFP_KERNEL);
	if (!npu)
		return -ENOMEM;

	npu->nmmu_flush = of_property_read_bool(hose->dn, "ibm,nmmu-flush");

	for (i = 0; i < ARRAY_SIZE(npu->mmio_atsd_regs) &&
			!of_property_read_u64_index(hose->dn, "ibm,mmio-atsd",
				i, &mmio_atsd); i++)
		npu->mmio_atsd_regs[i] = ioremap(mmio_atsd, 32);

	pr_info("NPU%d: Found %d MMIO ATSD registers", hose->global_number, i);
	npu->mmio_atsd_count = i;
	npu->mmio_atsd_usage = 0;
	npu_index++;
	if (WARN_ON(npu_index >= NV_MAX_NPUS)) {
		ret = -ENOSPC;
		goto fail_exit;
	}
	max_npu2_index = npu_index;
	npu->index = npu_index;
	hose->npu = npu;

	return 0;

fail_exit:
	for (i = 0; i < npu->mmio_atsd_count; ++i)
		iounmap(npu->mmio_atsd_regs[i]);

	kfree(npu);

	return ret;
}

int pnv_npu2_map_lpar_dev(struct pci_dev *gpdev, unsigned int lparid,
		unsigned long msr)
{
	int ret;
	struct pci_dev *npdev = pnv_pci_get_npu_dev(gpdev, 0);
	struct pci_controller *hose;
	struct pnv_phb *nphb;

	if (!npdev)
		return -ENODEV;

	hose = pci_bus_to_host(npdev->bus);
	nphb = hose->private_data;

	dev_dbg(&gpdev->dev, "Map LPAR opalid=%llu lparid=%u\n",
			nphb->opal_id, lparid);
	/*
	 * Currently we only support radix and non-zero LPCR only makes sense
	 * for hash tables so skiboot expects the LPCR parameter to be a zero.
	 */
	ret = opal_npu_map_lpar(nphb->opal_id,
			PCI_DEVID(gpdev->bus->number, gpdev->devfn), lparid,
			0 /* LPCR bits */);
	if (ret) {
		dev_err(&gpdev->dev, "Error %d mapping device to LPAR\n", ret);
		return ret;
	}

	dev_dbg(&gpdev->dev, "init context opalid=%llu msr=%lx\n",
			nphb->opal_id, msr);
	ret = opal_npu_init_context(nphb->opal_id, 0/*__unused*/, msr,
			PCI_DEVID(gpdev->bus->number, gpdev->devfn));
	if (ret < 0)
		dev_err(&gpdev->dev, "Failed to init context: %d\n", ret);
	else
		ret = 0;

	return 0;
}
EXPORT_SYMBOL_GPL(pnv_npu2_map_lpar_dev);

void pnv_npu2_map_lpar(struct pnv_ioda_pe *gpe, unsigned long msr)
{
	struct pci_dev *gpdev;

	list_for_each_entry(gpdev, &gpe->pbus->devices, bus_list)
		pnv_npu2_map_lpar_dev(gpdev, 0, msr);
}

int pnv_npu2_unmap_lpar_dev(struct pci_dev *gpdev)
{
	int ret;
	struct pci_dev *npdev = pnv_pci_get_npu_dev(gpdev, 0);
	struct pci_controller *hose;
	struct pnv_phb *nphb;

	if (!npdev)
		return -ENODEV;

	hose = pci_bus_to_host(npdev->bus);
	nphb = hose->private_data;

	dev_dbg(&gpdev->dev, "destroy context opalid=%llu\n",
			nphb->opal_id);
	ret = opal_npu_destroy_context(nphb->opal_id, 0/*__unused*/,
			PCI_DEVID(gpdev->bus->number, gpdev->devfn));
	if (ret < 0) {
		dev_err(&gpdev->dev, "Failed to destroy context: %d\n", ret);
		return ret;
	}

	/* Set LPID to 0 anyway, just to be safe */
	dev_dbg(&gpdev->dev, "Map LPAR opalid=%llu lparid=0\n", nphb->opal_id);
	ret = opal_npu_map_lpar(nphb->opal_id,
			PCI_DEVID(gpdev->bus->number, gpdev->devfn), 0 /*LPID*/,
			0 /* LPCR bits */);
	if (ret)
		dev_err(&gpdev->dev, "Error %d mapping device to LPAR\n", ret);

	return ret;
}
EXPORT_SYMBOL_GPL(pnv_npu2_unmap_lpar_dev);
