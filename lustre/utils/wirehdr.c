/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <linux/lustre/lustre_idl.h>
#include <linux/lustre/lustre_access_log.h>
#ifdef HAVE_SERVER_SUPPORT
#include <linux/lustre/lustre_lfsck_user.h>
#include <linux/lustre/lustre_disk.h>
#ifdef CONFIG_FS_POSIX_ACL
#include <linux/posix_acl_xattr.h>
#ifdef HAVE_STRUCT_POSIX_ACL_XATTR
# define posix_acl_xattr_header struct posix_acl_xattr_header
# define posix_acl_xattr_entry  struct posix_acl_xattr_entry
#endif /* HAVE_STRUCT_POSIX_ACL_XATTR */
#endif /* CONFIG_FS_POSIX_ACL */
#endif /* HAVE_SERVER_SUPPORT */
#include <linux/lustre/lustre_cfg.h>

#define LASSERT(cond) if (!(cond)) { printf("failed " #cond "\n"); ret = 1; }
#define LASSERTF(cond, fmt, ...) if (!(cond)) { printf("failed '" #cond "'" fmt, ## __VA_ARGS__); ret = 1; }
/*
 * BUILD_BUG_ON() is Compile-time check which verifies correctness at
 * compile-time rather than runtime. If "cond" is true, then there are two
 * identical cases ("0" and "0"), which is an error that causes the compiler to
 * complain. If "cond" is false, then there are two different cases
 * ("(non-zero)" and "0").
 *
 */
#ifndef BUILD_BUG_ON
#define BUILD_BUG_ON(cond) do {switch (0) {case (cond): case 1: break; } } while (0)
#endif

int ret;

void lustre_assert_wire_constants(void);

int main()
{
	lustre_assert_wire_constants();

	if (ret == 0)
		printf("wire constants OK\n");

	return ret;
}
