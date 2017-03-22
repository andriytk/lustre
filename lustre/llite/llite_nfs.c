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
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2014, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/lustre/llite/llite_nfs.c
 *
 * NFS export of Lustre Light File System
 *
 * Author: Yury Umanets <umka@clusterfs.com>
 * Author: Huang Hua <huanghua@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_LLITE
#include "llite_internal.h"
#include <linux/exportfs.h>

__u32 get_uuid2int(const char *name, int len)
{
        __u32 key0 = 0x12a3fe2d, key1 = 0x37abe8f9;
        while (len--) {
                __u32 key = key1 + (key0 ^ (*name++ * 7152373));
                if (key & 0x80000000) key -= 0x7fffffff;
                key1 = key0;
                key0 = key;
        }
        return (key0 << 1);
}

struct inode *search_inode_for_lustre(struct super_block *sb,
				      const struct lu_fid *fid)
{
        struct ll_sb_info     *sbi = ll_s2sbi(sb);
        struct ptlrpc_request *req = NULL;
        struct inode          *inode = NULL;
        int                   eadatalen = 0;
	unsigned long	      hash = cl_fid_build_ino(fid,
						      ll_need_32bit_api(sbi));
        struct  md_op_data    *op_data;
        int                   rc;
        ENTRY;

        CDEBUG(D_INFO, "searching inode for:(%lu,"DFID")\n", hash, PFID(fid));

	inode = ilookup5(sb, hash, ll_test_inode_by_fid, (void *)fid);
	if (inode)
		RETURN(inode);

	rc = ll_get_default_mdsize(sbi, &eadatalen);
	if (rc)
		RETURN(ERR_PTR(rc));

        /* Because inode is NULL, ll_prep_md_op_data can not
         * be used here. So we allocate op_data ourselves */
        OBD_ALLOC_PTR(op_data);
        if (op_data == NULL)
                return ERR_PTR(-ENOMEM);

        op_data->op_fid1 = *fid;
        op_data->op_mode = eadatalen;
        op_data->op_valid = OBD_MD_FLEASIZE;

        /* mds_fid2dentry ignores f_type */
        rc = md_getattr(sbi->ll_md_exp, op_data, &req);
        OBD_FREE_PTR(op_data);
        if (rc) {
		/* Suppress erroneous/confusing messages when NFS
		 * is out of sync and requests old data. */
		CDEBUG(D_INFO, "can't get object attrs, fid "DFID", rc %d\n",
				PFID(fid), rc);
                RETURN(ERR_PTR(rc));
        }
        rc = ll_prep_inode(&inode, req, sb, NULL);
        ptlrpc_req_finished(req);
        if (rc)
                RETURN(ERR_PTR(rc));

        RETURN(inode);
}

struct lustre_nfs_fid {
        struct lu_fid   lnf_child;
        struct lu_fid   lnf_parent;
};

static struct dentry *
ll_iget_for_nfs(struct super_block *sb, struct lu_fid *fid, struct lu_fid *parent)
{
        struct inode  *inode;
        struct dentry *result;
        ENTRY;

	if (!fid_is_sane(fid))
		RETURN(ERR_PTR(-ESTALE));

	CDEBUG(D_INFO, "Get dentry for fid: "DFID"\n", PFID(fid));

        inode = search_inode_for_lustre(sb, fid);
        if (IS_ERR(inode))
                RETURN(ERR_PTR(PTR_ERR(inode)));

        if (is_bad_inode(inode)) {
                /* we didn't find the right inode.. */
                iput(inode);
                RETURN(ERR_PTR(-ESTALE));
        }

	/* N.B. d_obtain_alias() drops inode ref on error */
	result = d_obtain_alias(inode);
	if (!IS_ERR(result)) {
		int rc;

		rc = ll_d_init(result);
		if (rc < 0) {
			dput(result);
			result = ERR_PTR(rc);
		} else {
			struct ll_dentry_data *ldd = ll_d2d(result);

			/*
			 * Need to signal to the ll_file_open that
			 * we came from NFS and so opencache needs to be
			 * enabled for this one
			 */
			spin_lock(&result->d_lock);
			ldd->lld_nfs_dentry = 1;
			spin_unlock(&result->d_lock);
		}
	}

	RETURN(result);
}

#ifndef FILEID_INVALID
#define FILEID_INVALID 0xff
#endif
#ifndef FILEID_LUSTRE
#define FILEID_LUSTRE  0x97
#endif

/**
 * \a connectable - is nfsd will connect himself or this should be done
 *                  at lustre
 *
 * The return value is file handle type:
 * 1 -- contains child file handle;
 * 2 -- contains child file handle and parent file handle;
 * 255 -- error.
 */
#ifndef HAVE_ENCODE_FH_PARENT
static int ll_encode_fh(struct dentry *de, __u32 *fh, int *plen,
			int connectable)
{
	struct inode *inode = de->d_inode;
	struct inode *parent = de->d_parent->d_inode;
#else
static int ll_encode_fh(struct inode *inode, __u32 *fh, int *plen,
			struct inode *parent)
{
#endif
	int fileid_len = sizeof(struct lustre_nfs_fid) / 4;
	struct lustre_nfs_fid *nfs_fid = (void *)fh;
	ENTRY;

	CDEBUG(D_INFO, "%s: encoding for ("DFID") maxlen=%d minlen=%d\n",
	       ll_get_fsname(inode->i_sb, NULL, 0),
	       PFID(ll_inode2fid(inode)), *plen, fileid_len);

	if (*plen < fileid_len) {
		*plen = fileid_len;
		RETURN(FILEID_INVALID);
	}

	nfs_fid->lnf_child = *ll_inode2fid(inode);
	if (parent != NULL)
		nfs_fid->lnf_parent = *ll_inode2fid(parent);
	else
		fid_zero(&nfs_fid->lnf_parent);
	*plen = fileid_len;

	RETURN(FILEID_LUSTRE);
}

static int
#ifndef HAVE_FILLDIR_USE_CTX
ll_nfs_get_name_filldir(void *cookie, const char *name, int namelen,
			loff_t hash, u64 ino, unsigned type)
{
	struct ll_getname_data *lgd = cookie;
#else
ll_nfs_get_name_filldir(struct dir_context *ctx, const char *name, int namelen,
			loff_t hash, u64 ino, unsigned type)
{
	struct ll_getname_data *lgd =
		container_of(ctx, struct ll_getname_data, ctx);
#endif /* HAVE_FILLDIR_USE_CTX */
        /* It is hack to access lde_fid for comparison with lgd_fid.
         * So the input 'name' must be part of the 'lu_dirent'. */
        struct lu_dirent *lde = container_of0(name, struct lu_dirent, lde_name);
        struct lu_fid fid;

        fid_le_to_cpu(&fid, &lde->lde_fid);
        if (lu_fid_eq(&fid, &lgd->lgd_fid)) {
                memcpy(lgd->lgd_name, name, namelen);
                lgd->lgd_name[namelen] = 0;
                lgd->lgd_found = 1;
        }
        return lgd->lgd_found;
}

static int ll_get_name(struct dentry *dentry, char *name,
                       struct dentry *child)
{
	struct inode *dir = dentry->d_inode;
	struct ll_getname_data lgd = {
		.lgd_name	= name,
		.lgd_fid	= ll_i2info(child->d_inode)->lli_fid,
#ifdef HAVE_DIR_CONTEXT
		.ctx.actor	= ll_nfs_get_name_filldir,
#endif
		.lgd_found = 0,
	};
	struct md_op_data *op_data;
	__u64	pos = 0;
	int rc;
	ENTRY;

        if (!dir || !S_ISDIR(dir->i_mode))
                GOTO(out, rc = -ENOTDIR);

        if (!dir->i_fop)
                GOTO(out, rc = -EINVAL);

	op_data = ll_prep_md_op_data(NULL, dir, dir, NULL, 0, 0,
				     LUSTRE_OPC_ANY, dir);
	if (IS_ERR(op_data))
		GOTO(out, rc = PTR_ERR(op_data));

	op_data->op_max_pages = ll_i2sbi(dir)->ll_md_brw_pages;
	mutex_lock(&dir->i_mutex);
#ifdef HAVE_DIR_CONTEXT
	rc = ll_dir_read(dir, &pos, op_data, &lgd.ctx);
#else
	rc = ll_dir_read(dir, &pos, op_data, &lgd, ll_nfs_get_name_filldir);
#endif
	mutex_unlock(&dir->i_mutex);
	ll_finish_md_op_data(op_data);
	if (!rc && !lgd.lgd_found)
		rc = -ENOENT;
	EXIT;
out:
	return rc;
}

static struct dentry *ll_fh_to_dentry(struct super_block *sb, struct fid *fid,
                                      int fh_len, int fh_type)
{
        struct lustre_nfs_fid *nfs_fid = (struct lustre_nfs_fid *)fid;

	if (fh_type != FILEID_LUSTRE)
		RETURN(ERR_PTR(-EPROTO));

        RETURN(ll_iget_for_nfs(sb, &nfs_fid->lnf_child, &nfs_fid->lnf_parent));
}

static struct dentry *ll_fh_to_parent(struct super_block *sb, struct fid *fid,
                                      int fh_len, int fh_type)
{
        struct lustre_nfs_fid *nfs_fid = (struct lustre_nfs_fid *)fid;

	if (fh_type != FILEID_LUSTRE)
		RETURN(ERR_PTR(-EPROTO));

        RETURN(ll_iget_for_nfs(sb, &nfs_fid->lnf_parent, NULL));
}

int ll_dir_get_parent_fid(struct inode *dir, struct lu_fid *parent_fid)
{
	struct ptlrpc_request	*req = NULL;
	struct ll_sb_info	*sbi;
	struct mdt_body		*body;
	static const char	dotdot[] = "..";
	struct md_op_data	*op_data;
	int			rc;
	int			lmmsize;
	ENTRY;

	LASSERT(dir && S_ISDIR(dir->i_mode));

	sbi = ll_s2sbi(dir->i_sb);

	CDEBUG(D_INFO, "%s: getting parent for ("DFID")\n",
	       ll_get_fsname(dir->i_sb, NULL, 0),
	       PFID(ll_inode2fid(dir)));

	rc = ll_get_default_mdsize(sbi, &lmmsize);
	if (rc != 0)
		RETURN(rc);

	op_data = ll_prep_md_op_data(NULL, dir, NULL, dotdot,
				     strlen(dotdot), lmmsize,
				     LUSTRE_OPC_ANY, NULL);
	if (IS_ERR(op_data))
		RETURN(PTR_ERR(op_data));

	rc = md_getattr_name(sbi->ll_md_exp, op_data, &req);
	ll_finish_md_op_data(op_data);
	if (rc != 0) {
		CERROR("%s: failure inode "DFID" get parent: rc = %d\n",
		       ll_get_fsname(dir->i_sb, NULL, 0),
		       PFID(ll_inode2fid(dir)), rc);
		RETURN(rc);
	}
	body = req_capsule_server_get(&req->rq_pill, &RMF_MDT_BODY);

	/*
	 * LU-3952: MDT may lost the FID of its parent, we should not crash
	 * the NFS server, ll_iget_for_nfs() will handle the error.
	 */
	if (body->mbo_valid & OBD_MD_FLID) {
		CDEBUG(D_INFO, "parent for "DFID" is "DFID"\n",
		       PFID(ll_inode2fid(dir)), PFID(&body->mbo_fid1));
		*parent_fid = body->mbo_fid1;
	}

	ptlrpc_req_finished(req);
	RETURN(0);
}

static struct dentry *ll_get_parent(struct dentry *dchild)
{
	struct lu_fid	parent_fid = { 0 };
	int		rc;
	struct dentry	*dentry;
	ENTRY;

	rc = ll_dir_get_parent_fid(dchild->d_inode, &parent_fid);
	if (rc != 0)
		RETURN(ERR_PTR(rc));

	dentry = ll_iget_for_nfs(dchild->d_inode->i_sb, &parent_fid, NULL);

	RETURN(dentry);
}

struct export_operations lustre_export_operations = {
       .get_parent = ll_get_parent,
       .encode_fh  = ll_encode_fh,
       .get_name   = ll_get_name,
        .fh_to_dentry = ll_fh_to_dentry,
        .fh_to_parent = ll_fh_to_parent,
};
