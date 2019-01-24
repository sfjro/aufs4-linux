/*
 * Copyright (C) 2015 - ARM Ltd
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/irqflags.h>

#include <asm/kvm_hyp.h>
#include <asm/kvm_mmu.h>
#include <asm/tlbflush.h>

struct tlb_inv_context {
	unsigned long	flags;
	u64		tcr;
	u64		sctlr;
};

static void __hyp_text __tlb_switch_to_guest_vhe(struct kvm *kvm,
						 struct tlb_inv_context *cxt)
{
	u64 val;

	local_irq_save(cxt->flags);

	if (cpus_have_const_cap(ARM64_WORKAROUND_1165522)) {
		/*
		 * For CPUs that are affected by ARM erratum 1165522, we
		 * cannot trust stage-1 to be in a correct state at that
		 * point. Since we do not want to force a full load of the
		 * vcpu state, we prevent the EL1 page-table walker to
		 * allocate new TLBs. This is done by setting the EPD bits
		 * in the TCR_EL1 register. We also need to prevent it to
		 * allocate IPA->PA walks, so we enable the S1 MMU...
		 */
		val = cxt->tcr = read_sysreg_el1(tcr);
		val |= TCR_EPD1_MASK | TCR_EPD0_MASK;
		write_sysreg_el1(val, tcr);
		val = cxt->sctlr = read_sysreg_el1(sctlr);
		val |= SCTLR_ELx_M;
		write_sysreg_el1(val, sctlr);
	}

	/*
	 * With VHE enabled, we have HCR_EL2.{E2H,TGE} = {1,1}, and
	 * most TLB operations target EL2/EL0. In order to affect the
	 * guest TLBs (EL1/EL0), we need to change one of these two
	 * bits. Changing E2H is impossible (goodbye TTBR1_EL2), so
	 * let's flip TGE before executing the TLB operation.
	 *
	 * ARM erratum 1165522 requires some special handling (again),
	 * as we need to make sure both stages of translation are in
	 * place before clearing TGE. __load_guest_stage2() already
	 * has an ISB in order to deal with this.
	 */
	__load_guest_stage2(kvm);
	val = read_sysreg(hcr_el2);
	val &= ~HCR_TGE;
	write_sysreg(val, hcr_el2);
	isb();
}

static void __hyp_text __tlb_switch_to_guest_nvhe(struct kvm *kvm,
						  struct tlb_inv_context *cxt)
{
	__load_guest_stage2(kvm);
	isb();
}

static hyp_alternate_select(__tlb_switch_to_guest,
			    __tlb_switch_to_guest_nvhe,
			    __tlb_switch_to_guest_vhe,
			    ARM64_HAS_VIRT_HOST_EXTN);

static void __hyp_text __tlb_switch_to_host_vhe(struct kvm *kvm,
						struct tlb_inv_context *cxt)
{
	/*
	 * We're done with the TLB operation, let's restore the host's
	 * view of HCR_EL2.
	 */
	write_sysreg(0, vttbr_el2);
	write_sysreg(HCR_HOST_VHE_FLAGS, hcr_el2);
	isb();

	if (cpus_have_const_cap(ARM64_WORKAROUND_1165522)) {
		/* Restore the registers to what they were */
		write_sysreg_el1(cxt->tcr, tcr);
		write_sysreg_el1(cxt->sctlr, sctlr);
	}

	local_irq_restore(cxt->flags);
}

static void __hyp_text __tlb_switch_to_host_nvhe(struct kvm *kvm,
						 struct tlb_inv_context *cxt)
{
	write_sysreg(0, vttbr_el2);
}

static hyp_alternate_select(__tlb_switch_to_host,
			    __tlb_switch_to_host_nvhe,
			    __tlb_switch_to_host_vhe,
			    ARM64_HAS_VIRT_HOST_EXTN);

void __hyp_text __kvm_tlb_flush_vmid_ipa(struct kvm *kvm, phys_addr_t ipa)
{
	struct tlb_inv_context cxt;

	dsb(ishst);

	/* Switch to requested VMID */
	kvm = kern_hyp_va(kvm);
	__tlb_switch_to_guest()(kvm, &cxt);

	/*
	 * We could do so much better if we had the VA as well.
	 * Instead, we invalidate Stage-2 for this IPA, and the
	 * whole of Stage-1. Weep...
	 */
	ipa >>= 12;
	__tlbi(ipas2e1is, ipa);

	/*
	 * We have to ensure completion of the invalidation at Stage-2,
	 * since a table walk on another CPU could refill a TLB with a
	 * complete (S1 + S2) walk based on the old Stage-2 mapping if
	 * the Stage-1 invalidation happened first.
	 */
	dsb(ish);
	__tlbi(vmalle1is);
	dsb(ish);
	isb();

	/*
	 * If the host is running at EL1 and we have a VPIPT I-cache,
	 * then we must perform I-cache maintenance at EL2 in order for
	 * it to have an effect on the guest. Since the guest cannot hit
	 * I-cache lines allocated with a different VMID, we don't need
	 * to worry about junk out of guest reset (we nuke the I-cache on
	 * VMID rollover), but we do need to be careful when remapping
	 * executable pages for the same guest. This can happen when KSM
	 * takes a CoW fault on an executable page, copies the page into
	 * a page that was previously mapped in the guest and then needs
	 * to invalidate the guest view of the I-cache for that page
	 * from EL1. To solve this, we invalidate the entire I-cache when
	 * unmapping a page from a guest if we have a VPIPT I-cache but
	 * the host is running at EL1. As above, we could do better if
	 * we had the VA.
	 *
	 * The moral of this story is: if you have a VPIPT I-cache, then
	 * you should be running with VHE enabled.
	 */
	if (!has_vhe() && icache_is_vpipt())
		__flush_icache_all();

	__tlb_switch_to_host()(kvm, &cxt);
}

void __hyp_text __kvm_tlb_flush_vmid(struct kvm *kvm)
{
	struct tlb_inv_context cxt;

	dsb(ishst);

	/* Switch to requested VMID */
	kvm = kern_hyp_va(kvm);
	__tlb_switch_to_guest()(kvm, &cxt);

	__tlbi(vmalls12e1is);
	dsb(ish);
	isb();

	__tlb_switch_to_host()(kvm, &cxt);
}

void __hyp_text __kvm_tlb_flush_local_vmid(struct kvm_vcpu *vcpu)
{
	struct kvm *kvm = kern_hyp_va(kern_hyp_va(vcpu)->kvm);
	struct tlb_inv_context cxt;

	/* Switch to requested VMID */
	__tlb_switch_to_guest()(kvm, &cxt);

	__tlbi(vmalle1);
	dsb(nsh);
	isb();

	__tlb_switch_to_host()(kvm, &cxt);
}

void __hyp_text __kvm_flush_vm_context(void)
{
	dsb(ishst);
	__tlbi(alle1is);
	asm volatile("ic ialluis" : : );
	dsb(ish);
}
