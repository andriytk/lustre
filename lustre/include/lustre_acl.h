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
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/include/lustre_acl.h
 */

#ifndef _LUSTRE_ACL_H
#define _LUSTRE_ACL_H

#ifdef __KERNEL__
# include <linux/fs.h>
# include <linux/dcache.h>
# ifdef CONFIG_FS_POSIX_ACL
#  include <linux/posix_acl_xattr.h>
#  define LUSTRE_POSIX_ACL_MAX_ENTRIES 32
#  define LUSTRE_POSIX_ACL_MAX_SIZE					\
	(sizeof(posix_acl_xattr_header) +				\
	 LUSTRE_POSIX_ACL_MAX_ENTRIES * sizeof(posix_acl_xattr_entry))
# endif /* CONFIG_FS_POSIX_ACL */
#endif /* __KERNEL__ */

#ifndef LUSTRE_POSIX_ACL_MAX_SIZE
# define LUSTRE_POSIX_ACL_MAX_SIZE 0
#endif /* LUSTRE_POSIX_ACL_MAX_SIZE */

#endif
