/*
 * Copyright (C) 2005-2015 Junjiro R. Okajima
 *
 * This program, aufs is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * debug print functions
 */

#include "aufs.h"

/* Returns 0, or -errno.  arg is in kp->arg. */
static int param_atomic_t_set(const char *val, const struct kernel_param *kp)
{
	int err, n;

	err = kstrtoint(val, 0, &n);
	if (!err) {
		if (n > 0)
			au_debug_on();
		else
			au_debug_off();
	}
	return err;
}

/* Returns length written or -errno.  Buffer is 4k (ie. be short!) */
static int param_atomic_t_get(char *buffer, const struct kernel_param *kp)
{
	atomic_t *a;

	a = kp->arg;
	return sprintf(buffer, "%d", atomic_read(a));
}

static struct kernel_param_ops param_ops_atomic_t = {
	.set = param_atomic_t_set,
	.get = param_atomic_t_get
	/* void (*free)(void *arg) */
};

atomic_t aufs_debug = ATOMIC_INIT(0);
MODULE_PARM_DESC(debug, "debug print");
module_param_named(debug, aufs_debug, atomic_t, S_IRUGO | S_IWUSR | S_IWGRP);
