// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/ctype.h>
#include <asm/ebcdic.h>
#include <asm/sclp.h>
#include <asm/sections.h>
#include <asm/boot_data.h>
#include "boot.h"

char __bootdata(early_command_line)[COMMAND_LINE_SIZE];
struct ipl_parameter_block __bootdata(early_ipl_block);
int __bootdata(early_ipl_block_valid);

unsigned long __bootdata(memory_end);
int __bootdata(memory_end_set);
int __bootdata(noexec_disabled);

static inline int __diag308(unsigned long subcode, void *addr)
{
	register unsigned long _addr asm("0") = (unsigned long)addr;
	register unsigned long _rc asm("1") = 0;
	unsigned long reg1, reg2;
	psw_t old = S390_lowcore.program_new_psw;

	asm volatile(
		"	epsw	%0,%1\n"
		"	st	%0,%[psw_pgm]\n"
		"	st	%1,%[psw_pgm]+4\n"
		"	larl	%0,1f\n"
		"	stg	%0,%[psw_pgm]+8\n"
		"	diag	%[addr],%[subcode],0x308\n"
		"1:	nopr	%%r7\n"
		: "=&d" (reg1), "=&a" (reg2),
		  [psw_pgm] "=Q" (S390_lowcore.program_new_psw),
		  [addr] "+d" (_addr), "+d" (_rc)
		: [subcode] "d" (subcode)
		: "cc", "memory");
	S390_lowcore.program_new_psw = old;
	return _rc;
}

void store_ipl_parmblock(void)
{
	int rc;

	rc = __diag308(DIAG308_STORE, &early_ipl_block);
	if (rc == DIAG308_RC_OK &&
	    early_ipl_block.hdr.version <= IPL_MAX_SUPPORTED_VERSION)
		early_ipl_block_valid = 1;
}

static size_t scpdata_length(const char *buf, size_t count)
{
	while (count) {
		if (buf[count - 1] != '\0' && buf[count - 1] != ' ')
			break;
		count--;
	}
	return count;
}

static size_t ipl_block_get_ascii_scpdata(char *dest, size_t size,
					  const struct ipl_parameter_block *ipb)
{
	size_t count;
	size_t i;
	int has_lowercase;

	count = min(size - 1, scpdata_length(ipb->ipl_info.fcp.scp_data,
					     ipb->ipl_info.fcp.scp_data_len));
	if (!count)
		goto out;

	has_lowercase = 0;
	for (i = 0; i < count; i++) {
		if (!isascii(ipb->ipl_info.fcp.scp_data[i])) {
			count = 0;
			goto out;
		}
		if (!has_lowercase && islower(ipb->ipl_info.fcp.scp_data[i]))
			has_lowercase = 1;
	}

	if (has_lowercase)
		memcpy(dest, ipb->ipl_info.fcp.scp_data, count);
	else
		for (i = 0; i < count; i++)
			dest[i] = tolower(ipb->ipl_info.fcp.scp_data[i]);
out:
	dest[count] = '\0';
	return count;
}

static void append_ipl_block_parm(void)
{
	char *parm, *delim;
	size_t len, rc = 0;

	len = strlen(early_command_line);

	delim = early_command_line + len;    /* '\0' character position */
	parm = early_command_line + len + 1; /* append right after '\0' */

	switch (early_ipl_block.hdr.pbt) {
	case DIAG308_IPL_TYPE_CCW:
		rc = ipl_block_get_ascii_vmparm(
			parm, COMMAND_LINE_SIZE - len - 1, &early_ipl_block);
		break;
	case DIAG308_IPL_TYPE_FCP:
		rc = ipl_block_get_ascii_scpdata(
			parm, COMMAND_LINE_SIZE - len - 1, &early_ipl_block);
		break;
	}
	if (rc) {
		if (*parm == '=')
			memmove(early_command_line, parm + 1, rc);
		else
			*delim = ' '; /* replace '\0' with space */
	}
}

static inline int has_ebcdic_char(const char *str)
{
	int i;

	for (i = 0; str[i]; i++)
		if (str[i] & 0x80)
			return 1;
	return 0;
}

void setup_boot_command_line(void)
{
	COMMAND_LINE[ARCH_COMMAND_LINE_SIZE - 1] = 0;
	/* convert arch command line to ascii if necessary */
	if (has_ebcdic_char(COMMAND_LINE))
		EBCASC(COMMAND_LINE, ARCH_COMMAND_LINE_SIZE);
	/* copy arch command line */
	strcpy(early_command_line, strim(COMMAND_LINE));

	/* append IPL PARM data to the boot command line */
	if (early_ipl_block_valid)
		append_ipl_block_parm();
}

static char command_line_buf[COMMAND_LINE_SIZE] __section(.data);
static void parse_mem_opt(void)
{
	char *param, *val;
	bool enabled;
	char *args;
	int rc;

	args = strcpy(command_line_buf, early_command_line);
	while (*args) {
		args = next_arg(args, &param, &val);

		if (!strcmp(param, "mem")) {
			memory_end = memparse(val, NULL);
			memory_end_set = 1;
		}

		if (!strcmp(param, "noexec")) {
			rc = kstrtobool(val, &enabled);
			if (!rc && !enabled)
				noexec_disabled = 1;
		}
	}
}

void setup_memory_end(void)
{
	parse_mem_opt();
#ifdef CONFIG_CRASH_DUMP
	if (!OLDMEM_BASE && early_ipl_block_valid &&
	    early_ipl_block.hdr.pbt == DIAG308_IPL_TYPE_FCP &&
	    early_ipl_block.ipl_info.fcp.opt == DIAG308_IPL_OPT_DUMP) {
		if (!sclp_early_get_hsa_size(&memory_end) && memory_end)
			memory_end_set = 1;
	}
#endif
}
