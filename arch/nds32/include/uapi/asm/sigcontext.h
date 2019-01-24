// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2005-2017 Andes Technology Corporation

#ifndef _ASMNDS32_SIGCONTEXT_H
#define _ASMNDS32_SIGCONTEXT_H

/*
 * Signal context structure - contains all info to do with the state
 * before the signal handler was invoked.  Note: only add new entries
 * to the end of the structure.
 */
struct fpu_struct {
	unsigned long long fd_regs[32];
	unsigned long fpcsr;
	/*
	 * UDF_trap is used to recognize whether underflow trap is enabled
	 * or not. When UDF_trap == 1, this process will be traped and then
	 * get a SIGFPE signal when encountering an underflow exception.
	 * UDF_trap is only modified through setfputrap syscall. Therefore,
	 * UDF_trap needn't be saved or loaded to context in each context
	 * switch.
	 */
	unsigned long UDF_trap;
};

struct zol_struct {
	unsigned long nds32_lc;	/* $LC */
	unsigned long nds32_le;	/* $LE */
	unsigned long nds32_lb;	/* $LB */
};

struct sigcontext {
	unsigned long trap_no;
	unsigned long error_code;
	unsigned long oldmask;
	unsigned long nds32_r0;
	unsigned long nds32_r1;
	unsigned long nds32_r2;
	unsigned long nds32_r3;
	unsigned long nds32_r4;
	unsigned long nds32_r5;
	unsigned long nds32_r6;
	unsigned long nds32_r7;
	unsigned long nds32_r8;
	unsigned long nds32_r9;
	unsigned long nds32_r10;
	unsigned long nds32_r11;
	unsigned long nds32_r12;
	unsigned long nds32_r13;
	unsigned long nds32_r14;
	unsigned long nds32_r15;
	unsigned long nds32_r16;
	unsigned long nds32_r17;
	unsigned long nds32_r18;
	unsigned long nds32_r19;
	unsigned long nds32_r20;
	unsigned long nds32_r21;
	unsigned long nds32_r22;
	unsigned long nds32_r23;
	unsigned long nds32_r24;
	unsigned long nds32_r25;
	unsigned long nds32_fp;	/* $r28 */
	unsigned long nds32_gp;	/* $r29 */
	unsigned long nds32_lp;	/* $r30 */
	unsigned long nds32_sp;	/* $r31 */
	unsigned long nds32_ipc;
	unsigned long fault_address;
	unsigned long used_math_flag;
	/* FPU Registers */
	struct fpu_struct fpu;
	struct zol_struct zol;
};

#endif
