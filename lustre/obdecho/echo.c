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
 * Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2010, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/obdecho/echo.c
 *
 * Author: Peter Braam <braam@clusterfs.com>
 * Author: Andreas Dilger <adilger@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_ECHO

#include <obd_support.h>
#include <obd_class.h>
#include <lustre_debug.h>
#include <lustre_dlm.h>
#include <lprocfs_status.h>
#include <lu_target.h>

#include "echo_internal.h"

/* The echo objid needs to be below 2^32, because regular FID numbers are
 * limited to 2^32 objects in f_oid for the FID_SEQ_ECHO range. b=23335 */
#define ECHO_INIT_OID        0x10000000ULL
#define ECHO_HANDLE_MAGIC    0xabcd0123fedc9876ULL

#define ECHO_PERSISTENT_PAGES (ECHO_PERSISTENT_SIZE >> PAGE_SHIFT)
static struct page *echo_persistent_pages[ECHO_PERSISTENT_PAGES];

enum {
        LPROC_ECHO_READ_BYTES = 1,
        LPROC_ECHO_WRITE_BYTES = 2,
        LPROC_ECHO_LAST = LPROC_ECHO_WRITE_BYTES +1
};

static int echo_connect(const struct lu_env *env,
                        struct obd_export **exp, struct obd_device *obd,
                        struct obd_uuid *cluuid, struct obd_connect_data *data,
                        void *localdata)
{
	struct lustre_handle conn = { 0 };
	int rc;

	data->ocd_connect_flags &= ECHO_CONNECT_SUPPORTED;

	if (data->ocd_connect_flags & OBD_CONNECT_FLAGS2)
		data->ocd_connect_flags2 &= ECHO_CONNECT_SUPPORTED2;

	rc = class_connect(&conn, obd, cluuid);
	if (rc) {
		CERROR("can't connect %d\n", rc);
		return rc;
	}
	*exp = class_conn2export(&conn);

	return 0;
}

static int echo_disconnect(struct obd_export *exp)
{
        LASSERT (exp != NULL);

        return server_disconnect_export(exp);
}

static int echo_init_export(struct obd_export *exp)
{
        return ldlm_init_export(exp);
}

static int echo_destroy_export(struct obd_export *exp)
{
        ENTRY;

        target_destroy_export(exp);
        ldlm_destroy_export(exp);

        RETURN(0);
}

static u64 echo_next_id(struct obd_device *obddev)
{
	u64 id;

	spin_lock(&obddev->u.echo.eo_lock);
	id = ++obddev->u.echo.eo_lastino;
	spin_unlock(&obddev->u.echo.eo_lock);

	return id;
}

static int echo_create(struct tgt_session_info *tsi)
{
	struct obd_export       *exp = tsi->tsi_exp;
	struct obd_device *obd = class_exp2obd(exp);
	struct ost_body *repbody;
	struct obdo     *oa = &tsi->tsi_ost_body->oa;
	struct obdo             *rep_oa;
	u64                      seq = ostid_seq(&oa->o_oi);

	if (OBD_FAIL_CHECK(OBD_FAIL_OST_EROFS))
		return -EROFS;
	repbody = req_capsule_server_get(tsi->tsi_pill, &RMF_OST_BODY);
	if (repbody == NULL)
		return -ENOMEM;
	rep_oa = &repbody->oa;
	rep_oa->o_oi = oa->o_oi;

	LASSERT(seq >= FID_SEQ_OST_MDT0);
	LASSERT(oa->o_valid & OBD_MD_FLGROUP);

	if (!(oa->o_mode & S_IFMT)) {
		CERROR("echo obd: no type!\n");
		return -ENOENT;
	}

        if (!(oa->o_valid & OBD_MD_FLTYPE)) {
		CERROR("invalid o_valid %#llx\n", oa->o_valid);
		return -EINVAL;
	}

	ostid_set_seq_echo(&rep_oa->o_oi);
	if (ostid_set_id(&rep_oa->o_oi, echo_next_id(obd))) {
		CERROR("Bad %llu to set " DOSTID "\n",
		       echo_next_id(obd), POSTID(&rep_oa->o_oi));
		return -EINVAL;
	}
	rep_oa->o_valid = OBD_MD_FLID;

	return 0;
}

static int echo_destroy(struct tgt_session_info *tsi)
{
	struct obd_device *obd = class_exp2obd(tsi->tsi_exp);
	struct obdo *oa = &tsi->tsi_ost_body->oa;

        ENTRY;
        if (!obd) {
		CERROR("invalid client cookie %#llx\n",
		       tsi->tsi_exp->exp_handle.h_cookie);
                RETURN(-EINVAL);
        }

        if (!(oa->o_valid & OBD_MD_FLID)) {
		CERROR("obdo missing FLID valid flag: %#llx\n", oa->o_valid);
                RETURN(-EINVAL);
        }

	if (ostid_id(&oa->o_oi) > obd->u.echo.eo_lastino ||
	    ostid_id(&oa->o_oi) < ECHO_INIT_OID) {
		CERROR("bad destroy objid: "DOSTID"\n", POSTID(&oa->o_oi));
		RETURN(-EINVAL);
	}

        RETURN(0);
}

static int echo_getattr(const struct lu_env *env, struct obd_export *exp,
			struct obdo *oa)
{
	struct obd_device *obd = class_exp2obd(exp);
	u64 id = ostid_id(&oa->o_oi);

	ENTRY;
	if (!obd) {
		CERROR("invalid client cookie %#llx\n",
		       exp->exp_handle.h_cookie);
		RETURN(-EINVAL);
	}

	if (!(oa->o_valid & OBD_MD_FLID)) {
		CERROR("obdo missing FLID valid flag: %#llx\n", oa->o_valid);
		RETURN(-EINVAL);
	}

	obdo_cpy_md(oa, &obd->u.echo.eo_oa, oa->o_valid);
	ostid_set_seq_echo(&oa->o_oi);
	if (ostid_set_id(&oa->o_oi, id)) {
		CERROR("Bad %llu to set " DOSTID "\n",
		       id, POSTID(&oa->o_oi));
		RETURN(-EINVAL);
	}

	RETURN(0);
}

static int echo_setattr(const struct lu_env *env, struct obd_export *exp,
			struct obdo *oa)
{
	struct obd_device *obd = class_exp2obd(exp);

	ENTRY;
	if (!obd) {
		CERROR("invalid client cookie %#llx\n",
		       exp->exp_handle.h_cookie);
		RETURN(-EINVAL);
	}

	if (!(oa->o_valid & OBD_MD_FLID)) {
		CERROR("obdo missing FLID valid flag: %#llx\n", oa->o_valid);
		RETURN(-EINVAL);
	}

	obd->u.echo.eo_oa = *oa;

	RETURN(0);
}

#define OBD_FAIL_OST_WRITE_NET	OBD_FAIL_OST_BRW_NET
#define OBD_FAIL_OST_READ_NET	OBD_FAIL_OST_BRW_NET
#define OST_BRW_WRITE	OST_WRITE
#define OST_BRW_READ	OST_READ

static struct tgt_handler obdecho_tgt_handlers[] = {
	TGT_RPC_HANDLER(OST_FIRST_OPC,
			0, OST_CONNECT, tgt_connect,
			&RQF_CONNECT, LUSTRE_OBD_VERSION),
	TGT_RPC_HANDLER(OST_FIRST_OPC,
			0, OST_DISCONNECT,tgt_disconnect,
			&RQF_OST_DISCONNECT, LUSTRE_OBD_VERSION),
	TGT_OST_HDL(HABEO_REFERO | MUTABOR,
		    OST_CREATE, echo_create),
	TGT_OST_HDL(HABEO_REFERO | MUTABOR,
		    OST_DESTROY, echo_destroy),
	TGT_OST_HDL(HABEO_CORPUS|  MUTABOR,
		    OST_BRW_WRITE, tgt_brw_write),
	TGT_OST_HDL(HABEO_CORPUS| HABEO_REFERO,
		    OST_BRW_READ, tgt_brw_read),
};
static struct tgt_opc_slice obdecho_common_slice[] = {
	{
		.tos_opc_start	= OST_FIRST_OPC,
		.tos_opc_end    = OST_LAST_OPC,
		.tos_hs	        = obdecho_tgt_handlers
	}
};

struct obdecho_device {
	struct obd_device	*ec_obd;
	struct lu_target	ec_lut;
	struct echo_obd 	*ed_eo;
	struct dt_device	ec_dt_dev;
	struct lu_device	*ec_lu_dev;
};

static inline struct obdecho_device *obdecho_dev(struct lu_device *d)
{
	return container_of0(d, struct obdecho_device, ec_dt_dev.dd_lu_dev);
}

static struct lu_device * obdecho_device_free(const struct lu_env *env,
					      struct lu_device *d)
{
	struct obdecho_device *ed = obdecho_dev(d);
	struct obd_device *obd = ed->ec_dt_dev.dd_lu_dev.ld_obd;
	int leaked;

	lprocfs_obd_cleanup(obd);
	lprocfs_free_obd_stats(obd);

	ldlm_lock_decref(&obd->u.echo.eo_nl_lock, LCK_NL);

	/* XXX Bug 3413; wait for a bit to ensure the BL callback has
	* happened before calling ldlm_namespace_free() */
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout(cfs_time_seconds(1));

	ldlm_namespace_free(obd->obd_namespace, NULL, obd->obd_force);
	obd->obd_namespace = NULL;

	leaked = atomic_read(&obd->u.echo.eo_prep);
	if (leaked != 0)
		CERROR("%d prep/commitrw pages leaked\n", leaked);

	dt_device_fini(&ed->ec_dt_dev);
	OBD_FREE_PTR(ed);
	RETURN(NULL);
}

static struct lu_device *obdecho_device_fini(const struct lu_env *env,
					     struct lu_device *d)
{
	struct obdecho_device *ed = obdecho_dev(d);

	ENTRY;
	tgt_fini(env, &ed->ec_lut);
	RETURN(NULL);
}

static struct lu_device *obdecho_device_alloc(const struct lu_env *env,
					      struct lu_device_type *t,
					      struct lustre_cfg *cfg)
{
	struct obdecho_device *ed;
	struct lu_device   *lud;
	struct obd_device  *obd = NULL;
	__u64			lock_flags = 0;
	struct ldlm_res_id	res_id = {.name = {1}};
	char			ns_name[48];
	int rc;

	ENTRY;

	OBD_ALLOC_PTR(ed);
	if (ed == NULL)
		ERR_PTR(-ENOMEM);
	dt_device_init(&ed->ec_dt_dev, t);
	lud = &ed->ec_dt_dev.dd_lu_dev;
	obd = class_name2obd(lustre_cfg_string(cfg, 0));
	LASSERT(obd != NULL);

	obd->obd_replayable = 0;

	rc = tgt_init(env, &ed->ec_lut, obd, &ed->ec_dt_dev,
		      obdecho_common_slice,
		      OBD_FAIL_OBDECHO_ALL_REQUEST_NET,
		      OBD_FAIL_OBDECHO_ALL_REPLY_NET);
	if (rc)
		GOTO(err_tgt, rc);

	ed->ec_dt_dev.dd_lu_dev.ld_obd = obd;

	spin_lock_init(&obd->u.echo.eo_lock);
	obd->u.echo.eo_lastino = ECHO_INIT_OID;

	sprintf(ns_name, "echotgt-%s", obd->obd_uuid.uuid);
	obd->obd_namespace = ldlm_namespace_new(obd, ns_name,
                                                LDLM_NAMESPACE_SERVER,
                                                LDLM_NAMESPACE_MODEST,
                                                LDLM_NS_TYPE_OST);
	if (obd->obd_namespace == NULL)
		GOTO(err_ns, rc = -ENOMEM);

	rc = ldlm_cli_enqueue_local(obd->obd_namespace, &res_id, LDLM_PLAIN,
				    NULL, LCK_NL, &lock_flags, NULL,
				    ldlm_completion_ast,
				    NULL, NULL, 0,
				    LVB_T_NONE, NULL, &obd->u.echo.eo_nl_lock);
	if (rc)
		GOTO(err_en, rc);

	if (lprocfs_obd_setup(obd, true) == 0 &&
	    lprocfs_alloc_obd_stats(obd, LPROC_ECHO_LAST) == 0) {
			lprocfs_counter_init(obd->obd_stats, LPROC_ECHO_READ_BYTES,
					     LPROCFS_CNTR_AVGMINMAX,
					     "read_bytes", "bytes");
			lprocfs_counter_init(obd->obd_stats, LPROC_ECHO_WRITE_BYTES,
					     LPROCFS_CNTR_AVGMINMAX,
					     "write_bytes", "bytes");
	}
	ptlrpc_init_client(LDLM_CB_REQUEST_PORTAL, LDLM_CB_REPLY_PORTAL,
			   "echo_ldlm_cb_client", &obd->obd_ldlm_client);

	RETURN(lud);
err_en:
	ldlm_namespace_free(obd->obd_namespace, NULL, obd->obd_force);
	obd->obd_namespace = NULL;
err_ns:
	tgt_fini(env, &ed->ec_lut);
err_tgt:
	OBD_FREE_PTR(ed);
	return ERR_PTR(rc);
}

static void
echo_page_debug_setup(struct page *page, int rw, u64 id,
		      __u64 offset, int len)
{
	int   page_offset = offset & ~PAGE_MASK;
	char *addr        = ((char *)kmap(page)) + page_offset;

        if (len % OBD_ECHO_BLOCK_SIZE != 0)
                CERROR("Unexpected block size %d\n", len);

        while (len > 0) {
                if (rw & OBD_BRW_READ)
                        block_debug_setup(addr, OBD_ECHO_BLOCK_SIZE,
                                          offset, id);
                else
                        block_debug_setup(addr, OBD_ECHO_BLOCK_SIZE,
                                          0xecc0ecc0ecc0ecc0ULL,
                                          0xecc0ecc0ecc0ecc0ULL);

                addr   += OBD_ECHO_BLOCK_SIZE;
                offset += OBD_ECHO_BLOCK_SIZE;
                len    -= OBD_ECHO_BLOCK_SIZE;
        }

	kunmap(page);
}

static int
echo_page_debug_check(struct page *page, u64 id,
		      __u64 offset, int len)
{
	int   page_offset = offset & ~PAGE_MASK;
	char *addr        = ((char *)kmap(page)) + page_offset;
	int   rc          = 0;
	int   rc2;

        if (len % OBD_ECHO_BLOCK_SIZE != 0)
                CERROR("Unexpected block size %d\n", len);

        while (len > 0) {
                rc2 = block_debug_check("echo", addr, OBD_ECHO_BLOCK_SIZE,
                                        offset, id);

                if (rc2 != 0 && rc == 0)
                        rc = rc2;

                addr   += OBD_ECHO_BLOCK_SIZE;
                offset += OBD_ECHO_BLOCK_SIZE;
                len    -= OBD_ECHO_BLOCK_SIZE;
        }

	kunmap(page);

	return rc;
}

static int echo_map_nb_to_lb(struct obdo *oa, struct obd_ioobj *obj,
                             struct niobuf_remote *nb, int *pages,
                             struct niobuf_local *lb, int cmd, int *left)
{
	gfp_t gfp_mask = (ostid_id(&obj->ioo_oid) & 1) ?
			GFP_HIGHUSER : GFP_KERNEL;
	int ispersistent = ostid_id(&obj->ioo_oid) == ECHO_PERSISTENT_OBJID;
	int debug_setup = (!ispersistent &&
			   (oa->o_valid & OBD_MD_FLFLAGS) != 0 &&
			   (oa->o_flags & OBD_FL_DEBUG_CHECK) != 0);
	struct niobuf_local *res = lb;
	u64 offset = nb->rnb_offset;
	int len = nb->rnb_len;

	while (len > 0) {
		int plen = PAGE_SIZE - (offset & (PAGE_SIZE-1));
		if (len < plen)
			plen = len;

                /* check for local buf overflow */
                if (*left == 0)
                        return -EINVAL;

		res->lnb_file_offset = offset;
		res->lnb_len = plen;
		LASSERT((res->lnb_file_offset & ~PAGE_MASK) +
			res->lnb_len <= PAGE_SIZE);

		if (ispersistent &&
		    ((res->lnb_file_offset >> PAGE_SHIFT) <
		      ECHO_PERSISTENT_PAGES)) {
			res->lnb_page =
				echo_persistent_pages[res->lnb_file_offset >>
						      PAGE_SHIFT];
			/* Take extra ref so __free_pages() can be called OK */
			get_page(res->lnb_page);
		} else {
			res->lnb_page = alloc_page(gfp_mask);
			if (res->lnb_page == NULL) {
				CERROR("can't get page for id " DOSTID"\n",
				       POSTID(&obj->ioo_oid));
				return -ENOMEM;
			}
		}

		CDEBUG(D_PAGE, "$$$$ get page %p @ %llu for %d\n",
		       res->lnb_page, res->lnb_file_offset, res->lnb_len);

		if (cmd & OBD_BRW_READ)
			res->lnb_rc = res->lnb_len;

		if (debug_setup)
			echo_page_debug_setup(res->lnb_page, cmd,
					      ostid_id(&obj->ioo_oid),
					      res->lnb_file_offset,
					      res->lnb_len);

                offset += plen;
                len -= plen;
                res++;

                (*left)--;
                (*pages)++;
        }

        return 0;
}

static int echo_finalize_lb(struct obdo *oa, struct obd_ioobj *obj,
			    struct niobuf_remote *rb, int *pgs,
			    struct niobuf_local *lb, int verify)
{
	struct niobuf_local *res = lb;
	u64 start = rb->rnb_offset >> PAGE_SHIFT;
	u64 end   = (rb->rnb_offset + rb->rnb_len + PAGE_SIZE - 1) >>
		    PAGE_SHIFT;
	int     count  = (int)(end - start);
	int     rc     = 0;
	int     i;

	for (i = 0; i < count; i++, (*pgs) ++, res++) {
		struct page *page = res->lnb_page;
		void       *addr;

		if (page == NULL) {
			CERROR("null page objid %llu:%p, buf %d/%d\n",
			       ostid_id(&obj->ioo_oid), page, i,
			       obj->ioo_bufcnt);
			return -EFAULT;
		}

		addr = kmap(page);

		CDEBUG(D_PAGE, "$$$$ use page %p, addr %p@%llu\n",
		       res->lnb_page, addr, res->lnb_file_offset);

		if (verify) {
			int vrc = echo_page_debug_check(page,
							ostid_id(&obj->ioo_oid),
							res->lnb_file_offset,
							res->lnb_len);
			/* check all the pages always */
			if (vrc != 0 && rc == 0)
				rc = vrc;
		}

		kunmap(page);
		/* NB see comment above regarding persistent pages */
		__free_page(page);
	}

	return rc;
}

static int echo_preprw(const struct lu_env *env, int cmd,
		       struct obd_export *export, struct obdo *oa,
		       int objcount, struct obd_ioobj *obj,
		       struct niobuf_remote *nb, int *pages,
		       struct niobuf_local *res)
{
        struct obd_device *obd;
        int tot_bytes = 0;
        int rc = 0;
        int i, left;
        ENTRY;

        obd = export->exp_obd;
        if (obd == NULL)
                RETURN(-EINVAL);

        /* Temp fix to stop falling foul of osc_announce_cached() */
        oa->o_valid &= ~(OBD_MD_FLBLOCKS | OBD_MD_FLGRANT);

        memset(res, 0, sizeof(*res) * *pages);

        CDEBUG(D_PAGE, "%s %d obdos with %d IOs\n",
               cmd == OBD_BRW_READ ? "reading" : "writing", objcount, *pages);

        left = *pages;
        *pages = 0;

        for (i = 0; i < objcount; i++, obj++) {
                int j;

                for (j = 0 ; j < obj->ioo_bufcnt ; j++, nb++) {

                        rc = echo_map_nb_to_lb(oa, obj, nb, pages,
                                               res + *pages, cmd, &left);
                        if (rc)
                                GOTO(preprw_cleanup, rc);

			tot_bytes += nb->rnb_len;
                }
        }

	atomic_add(*pages, &obd->u.echo.eo_prep);

        if (cmd & OBD_BRW_READ)
                lprocfs_counter_add(obd->obd_stats, LPROC_ECHO_READ_BYTES,
                                    tot_bytes);
        else
                lprocfs_counter_add(obd->obd_stats, LPROC_ECHO_WRITE_BYTES,
                                    tot_bytes);

        CDEBUG(D_PAGE, "%d pages allocated after prep\n",
	       atomic_read(&obd->u.echo.eo_prep));

        RETURN(0);

preprw_cleanup:
        /* It is possible that we would rather handle errors by  allow
         * any already-set-up pages to complete, rather than tearing them
         * all down again.  I believe that this is what the in-kernel
         * prep/commit operations do.
         */
        CERROR("cleaning up %u pages (%d obdos)\n", *pages, objcount);
        for (i = 0; i < *pages; i++) {
		kunmap(res[i].lnb_page);
		/* NB if this is a persistent page, __free_page() will just
		 * lose the extra ref gained above */
		__free_page(res[i].lnb_page);
		res[i].lnb_page = NULL;
		atomic_dec(&obd->u.echo.eo_prep);
	}

	return rc;
}

static int echo_commitrw(const struct lu_env *env, int cmd,
			 struct obd_export *export, struct obdo *oa,
			 int objcount, struct obd_ioobj *obj,
			 struct niobuf_remote *rb, int niocount,
			 struct niobuf_local *res, int rc)
{
        struct obd_device *obd;
        int pgs = 0;
        int i;
        ENTRY;

        obd = export->exp_obd;
        if (obd == NULL)
                RETURN(-EINVAL);

        if (rc)
                GOTO(commitrw_cleanup, rc);

        if ((cmd & OBD_BRW_RWMASK) == OBD_BRW_READ) {
                CDEBUG(D_PAGE, "reading %d obdos with %d IOs\n",
                       objcount, niocount);
        } else {
                CDEBUG(D_PAGE, "writing %d obdos with %d IOs\n",
                       objcount, niocount);
        }

        if (niocount && res == NULL) {
                CERROR("NULL res niobuf with niocount %d\n", niocount);
                RETURN(-EINVAL);
        }

	for (i = 0; i < objcount; i++, obj++) {
		int verify = (rc == 0 &&
			     ostid_id(&obj->ioo_oid) != ECHO_PERSISTENT_OBJID &&
			      (oa->o_valid & OBD_MD_FLFLAGS) != 0 &&
			      (oa->o_flags & OBD_FL_DEBUG_CHECK) != 0);
		int j;

		for (j = 0 ; j < obj->ioo_bufcnt ; j++, rb++) {
			int vrc = echo_finalize_lb(oa, obj, rb, &pgs, &res[pgs],
						   verify);
			if (vrc == 0)
				continue;

			if (vrc == -EFAULT)
				GOTO(commitrw_cleanup, rc = vrc);

			if (rc == 0)
				rc = vrc;
		}

	}

	atomic_sub(pgs, &obd->u.echo.eo_prep);

        CDEBUG(D_PAGE, "%d pages remain after commit\n",
	       atomic_read(&obd->u.echo.eo_prep));
        RETURN(rc);

commitrw_cleanup:
	atomic_sub(pgs, &obd->u.echo.eo_prep);

	CERROR("cleaning up %d pages (%d obdos)\n",
	       niocount - pgs - 1, objcount);

	while (pgs < niocount) {
		struct page *page = res[pgs++].lnb_page;

		if (page == NULL)
			continue;

		/* NB see comment above regarding persistent pages */
		__free_page(page);
		atomic_dec(&obd->u.echo.eo_prep);
	}
	return rc;
}

struct obd_ops echo_obd_ops = {
	.o_owner           = THIS_MODULE,
	.o_connect         = echo_connect,
	.o_disconnect      = echo_disconnect,
	.o_init_export     = echo_init_export,
	.o_destroy_export  = echo_destroy_export,
	.o_getattr         = echo_getattr,
	.o_setattr         = echo_setattr,
	.o_preprw          = echo_preprw,
	.o_commitrw        = echo_commitrw
};

static const struct lu_device_type_operations obdecho_device_type_ops = {
	.ldto_device_alloc  = obdecho_device_alloc,
	.ldto_device_free   = obdecho_device_free,
	.ldto_device_fini   = obdecho_device_fini
};

static struct lu_device_type obdecho_device_type = {
	.ldt_tags     = LU_DEVICE_DT,
	.ldt_name     = LUSTRE_ECHO_NAME,
	.ldt_ops      = &obdecho_device_type_ops,
	.ldt_ctx_tags = LCT_DT_THREAD,
};

void echo_persistent_pages_fini(void)
{
	int i;

	for (i = 0; i < ECHO_PERSISTENT_PAGES; i++)
		if (echo_persistent_pages[i] != NULL) {
			__free_page(echo_persistent_pages[i]);
			echo_persistent_pages[i] = NULL;
		}
}

static int echo_persistent_pages_init(void)
{
	struct page *pg;
	int          i;

	for (i = 0; i < ECHO_PERSISTENT_PAGES; i++) {
		gfp_t gfp_mask = (i < ECHO_PERSISTENT_PAGES/2) ?
			GFP_KERNEL : GFP_HIGHUSER;

		pg = alloc_page(gfp_mask);
		if (pg == NULL) {
			echo_persistent_pages_fini();
			return -ENOMEM;
		}

		memset(kmap(pg), 0, PAGE_SIZE);
		kunmap(pg);

		echo_persistent_pages[i] = pg;
	}

	return 0;
}
int obdecho_srv_register()
{
	int rc;

	rc = echo_persistent_pages_init();
	if (rc != 0)
		return rc;

	rc = class_register_type(&echo_obd_ops, NULL, true, NULL,
			       LUSTRE_ECHO_NAME, &obdecho_device_type);
	if (rc != 0)
		echo_persistent_pages_fini();
	return rc;
}
