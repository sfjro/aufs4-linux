// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2018 Facebook */
#include <stddef.h>
#include <linux/bpf.h>
#include <linux/types.h>
#include "bpf_helpers.h"

struct bpf_map_def SEC("maps") mim_array = {
	.type = BPF_MAP_TYPE_ARRAY_OF_MAPS,
	.key_size = sizeof(int),
	/* must be sizeof(__u32) for map in map */
	.value_size = sizeof(__u32),
	.max_entries = 1,
	.map_flags = 0,
};

struct bpf_map_def SEC("maps") mim_hash = {
	.type = BPF_MAP_TYPE_HASH_OF_MAPS,
	.key_size = sizeof(int),
	/* must be sizeof(__u32) for map in map */
	.value_size = sizeof(__u32),
	.max_entries = 1,
	.map_flags = 0,
};

SEC("xdp_mimtest")
int xdp_mimtest0(struct xdp_md *ctx)
{
	int value = 123;
	int key = 0;
	void *map;

	map = bpf_map_lookup_elem(&mim_array, &key);
	if (!map)
		return XDP_DROP;

	bpf_map_update_elem(map, &key, &value, 0);

	map = bpf_map_lookup_elem(&mim_hash, &key);
	if (!map)
		return XDP_DROP;

	bpf_map_update_elem(map, &key, &value, 0);

	return XDP_PASS;
}

int _version SEC("version") = 1;
char _license[] SEC("license") = "GPL";
