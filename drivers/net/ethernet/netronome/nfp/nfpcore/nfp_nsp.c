// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)
/* Copyright (C) 2015-2018 Netronome Systems, Inc. */

/*
 * nfp_nsp.c
 * Author: Jakub Kicinski <jakub.kicinski@netronome.com>
 *         Jason McMullan <jason.mcmullan@netronome.com>
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#define NFP_SUBSYS "nfp_nsp"

#include "nfp.h"
#include "nfp_cpp.h"
#include "nfp_nsp.h"

#define NFP_NSP_TIMEOUT_DEFAULT	30
#define NFP_NSP_TIMEOUT_BOOT	30

/* Offsets relative to the CSR base */
#define NSP_STATUS		0x00
#define   NSP_STATUS_MAGIC	GENMASK_ULL(63, 48)
#define   NSP_STATUS_MAJOR	GENMASK_ULL(47, 44)
#define   NSP_STATUS_MINOR	GENMASK_ULL(43, 32)
#define   NSP_STATUS_CODE	GENMASK_ULL(31, 16)
#define   NSP_STATUS_RESULT	GENMASK_ULL(15, 8)
#define   NSP_STATUS_BUSY	BIT_ULL(0)

#define NSP_COMMAND		0x08
#define   NSP_COMMAND_OPTION	GENMASK_ULL(63, 32)
#define   NSP_COMMAND_CODE	GENMASK_ULL(31, 16)
#define   NSP_COMMAND_START	BIT_ULL(0)

/* CPP address to retrieve the data from */
#define NSP_BUFFER		0x10
#define   NSP_BUFFER_CPP	GENMASK_ULL(63, 40)
#define   NSP_BUFFER_ADDRESS	GENMASK_ULL(39, 0)

#define NSP_DFLT_BUFFER		0x18
#define   NSP_DFLT_BUFFER_CPP	GENMASK_ULL(63, 40)
#define   NSP_DFLT_BUFFER_ADDRESS	GENMASK_ULL(39, 0)

#define NSP_DFLT_BUFFER_CONFIG	0x20
#define   NSP_DFLT_BUFFER_SIZE_MB	GENMASK_ULL(7, 0)

#define NSP_MAGIC		0xab10
#define NSP_MAJOR		0
#define NSP_MINOR		8

#define NSP_CODE_MAJOR		GENMASK(15, 12)
#define NSP_CODE_MINOR		GENMASK(11, 0)

#define NFP_FW_LOAD_RET_MAJOR	GENMASK(15, 8)
#define NFP_FW_LOAD_RET_MINOR	GENMASK(23, 16)

#define NFP_HWINFO_LOOKUP_SIZE	GENMASK(11, 0)

enum nfp_nsp_cmd {
	SPCODE_NOOP		= 0, /* No operation */
	SPCODE_SOFT_RESET	= 1, /* Soft reset the NFP */
	SPCODE_FW_DEFAULT	= 2, /* Load default (UNDI) FW */
	SPCODE_PHY_INIT		= 3, /* Initialize the PHY */
	SPCODE_MAC_INIT		= 4, /* Initialize the MAC */
	SPCODE_PHY_RXADAPT	= 5, /* Re-run PHY RX Adaptation */
	SPCODE_FW_LOAD		= 6, /* Load fw from buffer, len in option */
	SPCODE_ETH_RESCAN	= 7, /* Rescan ETHs, write ETH_TABLE to buf */
	SPCODE_ETH_CONTROL	= 8, /* Update media config from buffer */
	SPCODE_NSP_WRITE_FLASH	= 11, /* Load and flash image from buffer */
	SPCODE_NSP_SENSORS	= 12, /* Read NSP sensor(s) */
	SPCODE_NSP_IDENTIFY	= 13, /* Read NSP version */
	SPCODE_FW_STORED	= 16, /* If no FW loaded, load flash app FW */
	SPCODE_HWINFO_LOOKUP	= 17, /* Lookup HWinfo with overwrites etc. */
};

static const struct {
	int code;
	const char *msg;
} nsp_errors[] = {
	{ 6010, "could not map to phy for port" },
	{ 6011, "not an allowed rate/lanes for port" },
	{ 6012, "not an allowed rate/lanes for port" },
	{ 6013, "high/low error, change other port first" },
	{ 6014, "config not found in flash" },
};

struct nfp_nsp {
	struct nfp_cpp *cpp;
	struct nfp_resource *res;
	struct {
		u16 major;
		u16 minor;
	} ver;

	/* Eth table config state */
	bool modified;
	unsigned int idx;
	void *entries;
};

/**
 * struct nfp_nsp_command_arg - NFP command argument structure
 * @code:	NFP SP Command Code
 * @timeout_sec:Timeout value to wait for completion in seconds
 * @option:	NFP SP Command Argument
 * @buff_cpp:	NFP SP Buffer CPP Address info
 * @buff_addr:	NFP SP Buffer Host address
 * @error_cb:	Callback for interpreting option if error occurred
 */
struct nfp_nsp_command_arg {
	u16 code;
	unsigned int timeout_sec;
	u32 option;
	u32 buff_cpp;
	u64 buff_addr;
	void (*error_cb)(struct nfp_nsp *state, u32 ret_val);
};

/**
 * struct nfp_nsp_command_buf_arg - NFP command with buffer argument structure
 * @arg:	NFP command argument structure
 * @in_buf:	Buffer with data for input
 * @in_size:	Size of @in_buf
 * @out_buf:	Buffer for output data
 * @out_size:	Size of @out_buf
 */
struct nfp_nsp_command_buf_arg {
	struct nfp_nsp_command_arg arg;
	const void *in_buf;
	unsigned int in_size;
	void *out_buf;
	unsigned int out_size;
};

struct nfp_cpp *nfp_nsp_cpp(struct nfp_nsp *state)
{
	return state->cpp;
}

bool nfp_nsp_config_modified(struct nfp_nsp *state)
{
	return state->modified;
}

void nfp_nsp_config_set_modified(struct nfp_nsp *state, bool modified)
{
	state->modified = modified;
}

void *nfp_nsp_config_entries(struct nfp_nsp *state)
{
	return state->entries;
}

unsigned int nfp_nsp_config_idx(struct nfp_nsp *state)
{
	return state->idx;
}

void
nfp_nsp_config_set_state(struct nfp_nsp *state, void *entries, unsigned int idx)
{
	state->entries = entries;
	state->idx = idx;
}

void nfp_nsp_config_clear_state(struct nfp_nsp *state)
{
	state->entries = NULL;
	state->idx = 0;
}

static void nfp_nsp_print_extended_error(struct nfp_nsp *state, u32 ret_val)
{
	int i;

	if (!ret_val)
		return;

	for (i = 0; i < ARRAY_SIZE(nsp_errors); i++)
		if (ret_val == nsp_errors[i].code)
			nfp_err(state->cpp, "err msg: %s\n", nsp_errors[i].msg);
}

static int nfp_nsp_check(struct nfp_nsp *state)
{
	struct nfp_cpp *cpp = state->cpp;
	u64 nsp_status, reg;
	u32 nsp_cpp;
	int err;

	nsp_cpp = nfp_resource_cpp_id(state->res);
	nsp_status = nfp_resource_address(state->res) + NSP_STATUS;

	err = nfp_cpp_readq(cpp, nsp_cpp, nsp_status, &reg);
	if (err < 0)
		return err;

	if (FIELD_GET(NSP_STATUS_MAGIC, reg) != NSP_MAGIC) {
		nfp_err(cpp, "Cannot detect NFP Service Processor\n");
		return -ENODEV;
	}

	state->ver.major = FIELD_GET(NSP_STATUS_MAJOR, reg);
	state->ver.minor = FIELD_GET(NSP_STATUS_MINOR, reg);

	if (state->ver.major != NSP_MAJOR || state->ver.minor < NSP_MINOR) {
		nfp_err(cpp, "Unsupported ABI %hu.%hu\n",
			state->ver.major, state->ver.minor);
		return -EINVAL;
	}

	if (reg & NSP_STATUS_BUSY) {
		nfp_err(cpp, "Service processor busy!\n");
		return -EBUSY;
	}

	return 0;
}

/**
 * nfp_nsp_open() - Prepare for communication and lock the NSP resource.
 * @cpp:	NFP CPP Handle
 */
struct nfp_nsp *nfp_nsp_open(struct nfp_cpp *cpp)
{
	struct nfp_resource *res;
	struct nfp_nsp *state;
	int err;

	res = nfp_resource_acquire(cpp, NFP_RESOURCE_NSP);
	if (IS_ERR(res))
		return (void *)res;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state) {
		nfp_resource_release(res);
		return ERR_PTR(-ENOMEM);
	}
	state->cpp = cpp;
	state->res = res;

	err = nfp_nsp_check(state);
	if (err) {
		nfp_nsp_close(state);
		return ERR_PTR(err);
	}

	return state;
}

/**
 * nfp_nsp_close() - Clean up and unlock the NSP resource.
 * @state:	NFP SP state
 */
void nfp_nsp_close(struct nfp_nsp *state)
{
	nfp_resource_release(state->res);
	kfree(state);
}

u16 nfp_nsp_get_abi_ver_major(struct nfp_nsp *state)
{
	return state->ver.major;
}

u16 nfp_nsp_get_abi_ver_minor(struct nfp_nsp *state)
{
	return state->ver.minor;
}

static int
nfp_nsp_wait_reg(struct nfp_cpp *cpp, u64 *reg, u32 nsp_cpp, u64 addr,
		 u64 mask, u64 val, u32 timeout_sec)
{
	const unsigned long wait_until = jiffies + timeout_sec * HZ;
	int err;

	for (;;) {
		const unsigned long start_time = jiffies;

		err = nfp_cpp_readq(cpp, nsp_cpp, addr, reg);
		if (err < 0)
			return err;

		if ((*reg & mask) == val)
			return 0;

		msleep(25);

		if (time_after(start_time, wait_until))
			return -ETIMEDOUT;
	}
}

/**
 * __nfp_nsp_command() - Execute a command on the NFP Service Processor
 * @state:	NFP SP state
 * @arg:	NFP command argument structure
 *
 * Return: 0 for success with no result
 *
 *	 positive value for NSP completion with a result code
 *
 *	-EAGAIN if the NSP is not yet present
 *	-ENODEV if the NSP is not a supported model
 *	-EBUSY if the NSP is stuck
 *	-EINTR if interrupted while waiting for completion
 *	-ETIMEDOUT if the NSP took longer than @timeout_sec seconds to complete
 */
static int
__nfp_nsp_command(struct nfp_nsp *state, const struct nfp_nsp_command_arg *arg)
{
	u64 reg, ret_val, nsp_base, nsp_buffer, nsp_status, nsp_command;
	struct nfp_cpp *cpp = state->cpp;
	u32 nsp_cpp;
	int err;

	nsp_cpp = nfp_resource_cpp_id(state->res);
	nsp_base = nfp_resource_address(state->res);
	nsp_status = nsp_base + NSP_STATUS;
	nsp_command = nsp_base + NSP_COMMAND;
	nsp_buffer = nsp_base + NSP_BUFFER;

	err = nfp_nsp_check(state);
	if (err)
		return err;

	if (!FIELD_FIT(NSP_BUFFER_CPP, arg->buff_cpp >> 8) ||
	    !FIELD_FIT(NSP_BUFFER_ADDRESS, arg->buff_addr)) {
		nfp_err(cpp, "Host buffer out of reach %08x %016llx\n",
			arg->buff_cpp, arg->buff_addr);
		return -EINVAL;
	}

	err = nfp_cpp_writeq(cpp, nsp_cpp, nsp_buffer,
			     FIELD_PREP(NSP_BUFFER_CPP, arg->buff_cpp >> 8) |
			     FIELD_PREP(NSP_BUFFER_ADDRESS, arg->buff_addr));
	if (err < 0)
		return err;

	err = nfp_cpp_writeq(cpp, nsp_cpp, nsp_command,
			     FIELD_PREP(NSP_COMMAND_OPTION, arg->option) |
			     FIELD_PREP(NSP_COMMAND_CODE, arg->code) |
			     FIELD_PREP(NSP_COMMAND_START, 1));
	if (err < 0)
		return err;

	/* Wait for NSP_COMMAND_START to go to 0 */
	err = nfp_nsp_wait_reg(cpp, &reg, nsp_cpp, nsp_command,
			       NSP_COMMAND_START, 0, NFP_NSP_TIMEOUT_DEFAULT);
	if (err) {
		nfp_err(cpp, "Error %d waiting for code 0x%04x to start\n",
			err, arg->code);
		return err;
	}

	/* Wait for NSP_STATUS_BUSY to go to 0 */
	err = nfp_nsp_wait_reg(cpp, &reg, nsp_cpp, nsp_status, NSP_STATUS_BUSY,
			       0, arg->timeout_sec ?: NFP_NSP_TIMEOUT_DEFAULT);
	if (err) {
		nfp_err(cpp, "Error %d waiting for code 0x%04x to complete\n",
			err, arg->code);
		return err;
	}

	err = nfp_cpp_readq(cpp, nsp_cpp, nsp_command, &ret_val);
	if (err < 0)
		return err;
	ret_val = FIELD_GET(NSP_COMMAND_OPTION, ret_val);

	err = FIELD_GET(NSP_STATUS_RESULT, reg);
	if (err) {
		nfp_warn(cpp, "Result (error) code set: %d (%d) command: %d\n",
			 -err, (int)ret_val, arg->code);
		if (arg->error_cb)
			arg->error_cb(state, ret_val);
		else
			nfp_nsp_print_extended_error(state, ret_val);
		return -err;
	}

	return ret_val;
}

static int nfp_nsp_command(struct nfp_nsp *state, u16 code)
{
	const struct nfp_nsp_command_arg arg = {
		.code		= code,
	};

	return __nfp_nsp_command(state, &arg);
}

static int
nfp_nsp_command_buf(struct nfp_nsp *nsp, struct nfp_nsp_command_buf_arg *arg)
{
	struct nfp_cpp *cpp = nsp->cpp;
	unsigned int max_size;
	u64 reg, cpp_buf;
	int ret, err;
	u32 cpp_id;

	if (nsp->ver.minor < 13) {
		nfp_err(cpp, "NSP: Code 0x%04x with buffer not supported (ABI %hu.%hu)\n",
			arg->arg.code, nsp->ver.major, nsp->ver.minor);
		return -EOPNOTSUPP;
	}

	err = nfp_cpp_readq(cpp, nfp_resource_cpp_id(nsp->res),
			    nfp_resource_address(nsp->res) +
			    NSP_DFLT_BUFFER_CONFIG,
			    &reg);
	if (err < 0)
		return err;

	max_size = max(arg->in_size, arg->out_size);
	if (FIELD_GET(NSP_DFLT_BUFFER_SIZE_MB, reg) * SZ_1M < max_size) {
		nfp_err(cpp, "NSP: default buffer too small for command 0x%04x (%llu < %u)\n",
			arg->arg.code,
			FIELD_GET(NSP_DFLT_BUFFER_SIZE_MB, reg) * SZ_1M,
			max_size);
		return -EINVAL;
	}

	err = nfp_cpp_readq(cpp, nfp_resource_cpp_id(nsp->res),
			    nfp_resource_address(nsp->res) +
			    NSP_DFLT_BUFFER,
			    &reg);
	if (err < 0)
		return err;

	cpp_id = FIELD_GET(NSP_DFLT_BUFFER_CPP, reg) << 8;
	cpp_buf = FIELD_GET(NSP_DFLT_BUFFER_ADDRESS, reg);

	if (arg->in_buf && arg->in_size) {
		err = nfp_cpp_write(cpp, cpp_id, cpp_buf,
				    arg->in_buf, arg->in_size);
		if (err < 0)
			return err;
	}
	/* Zero out remaining part of the buffer */
	if (arg->out_buf && arg->out_size && arg->out_size > arg->in_size) {
		memset(arg->out_buf, 0, arg->out_size - arg->in_size);
		err = nfp_cpp_write(cpp, cpp_id, cpp_buf + arg->in_size,
				    arg->out_buf, arg->out_size - arg->in_size);
		if (err < 0)
			return err;
	}

	arg->arg.buff_cpp = cpp_id;
	arg->arg.buff_addr = cpp_buf;
	ret = __nfp_nsp_command(nsp, &arg->arg);
	if (ret < 0)
		return ret;

	if (arg->out_buf && arg->out_size) {
		err = nfp_cpp_read(cpp, cpp_id, cpp_buf,
				   arg->out_buf, arg->out_size);
		if (err < 0)
			return err;
	}

	return ret;
}

int nfp_nsp_wait(struct nfp_nsp *state)
{
	const unsigned long wait_until = jiffies + NFP_NSP_TIMEOUT_BOOT * HZ;
	int err;

	nfp_dbg(state->cpp, "Waiting for NSP to respond (%u sec max).\n",
		NFP_NSP_TIMEOUT_BOOT);

	for (;;) {
		const unsigned long start_time = jiffies;

		err = nfp_nsp_command(state, SPCODE_NOOP);
		if (err != -EAGAIN)
			break;

		if (msleep_interruptible(25)) {
			err = -ERESTARTSYS;
			break;
		}

		if (time_after(start_time, wait_until)) {
			err = -ETIMEDOUT;
			break;
		}
	}
	if (err)
		nfp_err(state->cpp, "NSP failed to respond %d\n", err);

	return err;
}

int nfp_nsp_device_soft_reset(struct nfp_nsp *state)
{
	return nfp_nsp_command(state, SPCODE_SOFT_RESET);
}

int nfp_nsp_mac_reinit(struct nfp_nsp *state)
{
	return nfp_nsp_command(state, SPCODE_MAC_INIT);
}

static void nfp_nsp_load_fw_extended_msg(struct nfp_nsp *state, u32 ret_val)
{
	static const char * const major_msg[] = {
		/* 0 */ "Firmware from driver loaded",
		/* 1 */ "Firmware from flash loaded",
		/* 2 */ "Firmware loading failure",
	};
	static const char * const minor_msg[] = {
		/*  0 */ "",
		/*  1 */ "no named partition on flash",
		/*  2 */ "error reading from flash",
		/*  3 */ "can not deflate",
		/*  4 */ "not a trusted file",
		/*  5 */ "can not parse FW file",
		/*  6 */ "MIP not found in FW file",
		/*  7 */ "null firmware name in MIP",
		/*  8 */ "FW version none",
		/*  9 */ "FW build number none",
		/* 10 */ "no FW selection policy HWInfo key found",
		/* 11 */ "static FW selection policy",
		/* 12 */ "FW version has precedence",
		/* 13 */ "different FW application load requested",
		/* 14 */ "development build",
	};
	unsigned int major, minor;
	const char *level;

	major = FIELD_GET(NFP_FW_LOAD_RET_MAJOR, ret_val);
	minor = FIELD_GET(NFP_FW_LOAD_RET_MINOR, ret_val);

	if (!nfp_nsp_has_stored_fw_load(state))
		return;

	/* Lower the message level in legacy case */
	if (major == 0 && (minor == 0 || minor == 10))
		level = KERN_DEBUG;
	else if (major == 2)
		level = KERN_ERR;
	else
		level = KERN_INFO;

	if (major >= ARRAY_SIZE(major_msg))
		nfp_printk(level, state->cpp, "FW loading status: %x\n",
			   ret_val);
	else if (minor >= ARRAY_SIZE(minor_msg))
		nfp_printk(level, state->cpp, "%s, reason code: %d\n",
			   major_msg[major], minor);
	else
		nfp_printk(level, state->cpp, "%s%c %s\n",
			   major_msg[major], minor ? ',' : '.',
			   minor_msg[minor]);
}

int nfp_nsp_load_fw(struct nfp_nsp *state, const struct firmware *fw)
{
	struct nfp_nsp_command_buf_arg load_fw = {
		{
			.code		= SPCODE_FW_LOAD,
			.option		= fw->size,
			.error_cb	= nfp_nsp_load_fw_extended_msg,
		},
		.in_buf		= fw->data,
		.in_size	= fw->size,
	};
	int ret;

	ret = nfp_nsp_command_buf(state, &load_fw);
	if (ret < 0)
		return ret;

	nfp_nsp_load_fw_extended_msg(state, ret);
	return 0;
}

int nfp_nsp_write_flash(struct nfp_nsp *state, const struct firmware *fw)
{
	struct nfp_nsp_command_buf_arg write_flash = {
		{
			.code		= SPCODE_NSP_WRITE_FLASH,
			.option		= fw->size,
			/* The flash time is specified to take a maximum of 70s
			 * so we add an additional factor to this spec time.
			 */
			.timeout_sec	= 2.5 * 70,
		},
		.in_buf		= fw->data,
		.in_size	= fw->size,
	};

	return nfp_nsp_command_buf(state, &write_flash);
}

int nfp_nsp_read_eth_table(struct nfp_nsp *state, void *buf, unsigned int size)
{
	struct nfp_nsp_command_buf_arg eth_rescan = {
		{
			.code		= SPCODE_ETH_RESCAN,
			.option		= size,
		},
		.out_buf	= buf,
		.out_size	= size,
	};

	return nfp_nsp_command_buf(state, &eth_rescan);
}

int nfp_nsp_write_eth_table(struct nfp_nsp *state,
			    const void *buf, unsigned int size)
{
	struct nfp_nsp_command_buf_arg eth_ctrl = {
		{
			.code		= SPCODE_ETH_CONTROL,
			.option		= size,
		},
		.in_buf		= buf,
		.in_size	= size,
	};

	return nfp_nsp_command_buf(state, &eth_ctrl);
}

int nfp_nsp_read_identify(struct nfp_nsp *state, void *buf, unsigned int size)
{
	struct nfp_nsp_command_buf_arg identify = {
		{
			.code		= SPCODE_NSP_IDENTIFY,
			.option		= size,
		},
		.out_buf	= buf,
		.out_size	= size,
	};

	return nfp_nsp_command_buf(state, &identify);
}

int nfp_nsp_read_sensors(struct nfp_nsp *state, unsigned int sensor_mask,
			 void *buf, unsigned int size)
{
	struct nfp_nsp_command_buf_arg sensors = {
		{
			.code		= SPCODE_NSP_SENSORS,
			.option		= sensor_mask,
		},
		.out_buf	= buf,
		.out_size	= size,
	};

	return nfp_nsp_command_buf(state, &sensors);
}

int nfp_nsp_load_stored_fw(struct nfp_nsp *state)
{
	const struct nfp_nsp_command_arg arg = {
		.code		= SPCODE_FW_STORED,
		.error_cb	= nfp_nsp_load_fw_extended_msg,
	};
	int ret;

	ret = __nfp_nsp_command(state, &arg);
	if (ret < 0)
		return ret;

	nfp_nsp_load_fw_extended_msg(state, ret);
	return 0;
}

static int
__nfp_nsp_hwinfo_lookup(struct nfp_nsp *state, void *buf, unsigned int size)
{
	struct nfp_nsp_command_buf_arg hwinfo_lookup = {
		{
			.code		= SPCODE_HWINFO_LOOKUP,
			.option		= size,
		},
		.in_buf		= buf,
		.in_size	= size,
		.out_buf	= buf,
		.out_size	= size,
	};

	return nfp_nsp_command_buf(state, &hwinfo_lookup);
}

int nfp_nsp_hwinfo_lookup(struct nfp_nsp *state, void *buf, unsigned int size)
{
	int err;

	size = min_t(u32, size, NFP_HWINFO_LOOKUP_SIZE);

	err = __nfp_nsp_hwinfo_lookup(state, buf, size);
	if (err)
		return err;

	if (strnlen(buf, size) == size) {
		nfp_err(state->cpp, "NSP HWinfo value not NULL-terminated\n");
		return -EINVAL;
	}

	return 0;
}
