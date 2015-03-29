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
 * mount options/flags
 */

#include <linux/parser.h>
#include "aufs.h"

static const char *au_optstr(int *val, match_table_t tbl)
{
	struct match_token *p;
	int v;

	v = *val;
	if (!v)
		goto out;
	p = tbl;
	while (p->pattern) {
		if (p->token
		    && (v & p->token) == p->token) {
			*val &= ~p->token;
			return p->pattern;
		}
		p++;
	}

out:
	return NULL;
}

/* ---------------------------------------------------------------------- */

static match_table_t brperm = {
	{AuBrPerm_RO, AUFS_BRPERM_RO},
	{0, NULL}
};

void au_optstr_br_perm(au_br_perm_str_t *str, int perm)
{
	const char *p;
	char *q;
	int sz;

	q = str->a;
	p = au_optstr(&perm, brperm);
	AuDebugOn(!p || !*p);
	sz = strlen(p);
	memcpy(q, p, sz + 1);
	q += sz;

	AuDebugOn(strlen(str->a) >= sizeof(str->a));
}
