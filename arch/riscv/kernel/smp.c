/*
 * SMP initialisation and IPI support
 * Based on arch/arm64/kernel/smp.c
 *
 * Copyright (C) 2012 ARM Ltd.
 * Copyright (C) 2015 Regents of the University of California
 * Copyright (C) 2017 SiFive
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

#include <linux/interrupt.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/seq_file.h>

#include <asm/sbi.h>
#include <asm/tlbflush.h>
#include <asm/cacheflush.h>

enum ipi_message_type {
	IPI_RESCHEDULE,
	IPI_CALL_FUNC,
	IPI_MAX
};

/* A collection of single bit ipi messages.  */
static struct {
	unsigned long stats[IPI_MAX] ____cacheline_aligned;
	unsigned long bits ____cacheline_aligned;
} ipi_data[NR_CPUS] __cacheline_aligned;

int riscv_hartid_to_cpuid(int hartid)
{
	int i = -1;

	for (i = 0; i < NR_CPUS; i++)
		if (cpuid_to_hartid_map(i) == hartid)
			return i;

	pr_err("Couldn't find cpu id for hartid [%d]\n", hartid);
	BUG();
	return i;
}

void riscv_cpuid_to_hartid_mask(const struct cpumask *in, struct cpumask *out)
{
	int cpu;

	for_each_cpu(cpu, in)
		cpumask_set_cpu(cpuid_to_hartid_map(cpu), out);
}
/* Unsupported */
int setup_profiling_timer(unsigned int multiplier)
{
	return -EINVAL;
}

void riscv_software_interrupt(void)
{
	unsigned long *pending_ipis = &ipi_data[smp_processor_id()].bits;
	unsigned long *stats = ipi_data[smp_processor_id()].stats;

	/* Clear pending IPI */
	csr_clear(sip, SIE_SSIE);

	while (true) {
		unsigned long ops;

		/* Order bit clearing and data access. */
		mb();

		ops = xchg(pending_ipis, 0);
		if (ops == 0)
			return;

		if (ops & (1 << IPI_RESCHEDULE)) {
			stats[IPI_RESCHEDULE]++;
			scheduler_ipi();
		}

		if (ops & (1 << IPI_CALL_FUNC)) {
			stats[IPI_CALL_FUNC]++;
			generic_smp_call_function_interrupt();
		}

		BUG_ON((ops >> IPI_MAX) != 0);

		/* Order data access and bit testing. */
		mb();
	}
}

static void
send_ipi_message(const struct cpumask *to_whom, enum ipi_message_type operation)
{
	int cpuid, hartid;
	struct cpumask hartid_mask;

	cpumask_clear(&hartid_mask);
	mb();
	for_each_cpu(cpuid, to_whom) {
		set_bit(operation, &ipi_data[cpuid].bits);
		hartid = cpuid_to_hartid_map(cpuid);
		cpumask_set_cpu(hartid, &hartid_mask);
	}
	mb();
	sbi_send_ipi(cpumask_bits(&hartid_mask));
}

static const char * const ipi_names[] = {
	[IPI_RESCHEDULE]	= "Rescheduling interrupts",
	[IPI_CALL_FUNC]		= "Function call interrupts",
};

void show_ipi_stats(struct seq_file *p, int prec)
{
	unsigned int cpu, i;

	for (i = 0; i < IPI_MAX; i++) {
		seq_printf(p, "%*s%u:%s", prec - 1, "IPI", i,
			   prec >= 4 ? " " : "");
		for_each_online_cpu(cpu)
			seq_printf(p, "%10lu ", ipi_data[cpu].stats[i]);
		seq_printf(p, " %s\n", ipi_names[i]);
	}
}

void arch_send_call_function_ipi_mask(struct cpumask *mask)
{
	send_ipi_message(mask, IPI_CALL_FUNC);
}

void arch_send_call_function_single_ipi(int cpu)
{
	send_ipi_message(cpumask_of(cpu), IPI_CALL_FUNC);
}

static void ipi_stop(void *unused)
{
	while (1)
		wait_for_interrupt();
}

void smp_send_stop(void)
{
	on_each_cpu(ipi_stop, NULL, 1);
}

void smp_send_reschedule(int cpu)
{
	send_ipi_message(cpumask_of(cpu), IPI_RESCHEDULE);
}

/*
 * Performs an icache flush for the given MM context.  RISC-V has no direct
 * mechanism for instruction cache shoot downs, so instead we send an IPI that
 * informs the remote harts they need to flush their local instruction caches.
 * To avoid pathologically slow behavior in a common case (a bunch of
 * single-hart processes on a many-hart machine, ie 'make -j') we avoid the
 * IPIs for harts that are not currently executing a MM context and instead
 * schedule a deferred local instruction cache flush to be performed before
 * execution resumes on each hart.
 */
void flush_icache_mm(struct mm_struct *mm, bool local)
{
	unsigned int cpu;
	cpumask_t others, hmask, *mask;

	preempt_disable();

	/* Mark every hart's icache as needing a flush for this MM. */
	mask = &mm->context.icache_stale_mask;
	cpumask_setall(mask);
	/* Flush this hart's I$ now, and mark it as flushed. */
	cpu = smp_processor_id();
	cpumask_clear_cpu(cpu, mask);
	local_flush_icache_all();

	/*
	 * Flush the I$ of other harts concurrently executing, and mark them as
	 * flushed.
	 */
	cpumask_andnot(&others, mm_cpumask(mm), cpumask_of(cpu));
	local |= cpumask_empty(&others);
	if (mm != current->active_mm || !local) {
		cpumask_clear(&hmask);
		riscv_cpuid_to_hartid_mask(&others, &hmask);
		sbi_remote_fence_i(hmask.bits);
	} else {
		/*
		 * It's assumed that at least one strongly ordered operation is
		 * performed on this hart between setting a hart's cpumask bit
		 * and scheduling this MM context on that hart.  Sending an SBI
		 * remote message will do this, but in the case where no
		 * messages are sent we still need to order this hart's writes
		 * with flush_icache_deferred().
		 */
		smp_mb();
	}

	preempt_enable();
}
