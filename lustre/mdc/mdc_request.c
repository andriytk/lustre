/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2001, 2002 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.sf.net/projects/lustre/
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#define EXPORT_SYMTAB
#define DEBUG_SUBSYSTEM S_MDC

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/lustre_mds.h>
#include <linux/lustre_lite.h>
#include <linux/lustre_dlm.h>

#define REQUEST_MINOR 244

extern int mds_queue_req(struct ptlrpc_request *);

int mdc_con2cl(struct lustre_handle *conn, struct ptlrpc_client **cl,
               struct ptlrpc_connection **connection,
               struct lustre_handle **rconn)
{
        struct obd_export *export;
        struct mdc_obd *mdc;

        export = class_conn2export(conn);
        if (!export)
                return -ENOTCONN;

        mdc = &export->exp_obd->u.mdc;

        *cl = mdc->mdc_client;
        *connection = mdc->mdc_conn;
        *rconn = &export->exp_rconnh;

        return 0;
}

static int mdc_con2dlmcl(struct lustre_handle *conn, struct ptlrpc_client **cl,
                         struct ptlrpc_connection **connection,
                         struct lustre_handle **rconn)
{
        struct obd_export *export;
        struct mdc_obd *mdc;

        export = class_conn2export(conn);
        if (!export)
                return -ENOTCONN;

        mdc = &export->exp_obd->u.mdc;

        *cl = mdc->mdc_ldlm_client;
        *connection = mdc->mdc_conn;
        *rconn = &export->exp_rconnh;

        return 0;
}


int mdc_getstatus(struct lustre_handle *conn, struct ll_fid *rootfid,
                  __u64 *last_committed, __u64 *last_rcvd,
                  __u32 *last_xid, struct ptlrpc_request **request)
{
        struct ptlrpc_request *req;
        struct mds_body *body;
        struct ptlrpc_client *cl;
        struct ptlrpc_connection *connection;
        struct lustre_handle *rconn;
        int rc, size = sizeof(*body);
        ENTRY;

        mdc_con2cl(conn, &cl, &connection, &rconn);
        req = ptlrpc_prep_req2(cl, connection, rconn,
                               MDS_GETSTATUS, 1, &size, NULL);
        if (!req)
                GOTO(out, rc = -ENOMEM);

        body = lustre_msg_buf(req->rq_reqmsg, 0);
        req->rq_level = LUSTRE_CONN_CON;
        req->rq_replen = lustre_msg_size(1, &size);

        mds_pack_req_body(req);
        rc = ptlrpc_queue_wait(req);
        rc = ptlrpc_check_status(req, rc);

        if (!rc) {
                body = lustre_msg_buf(req->rq_repmsg, 0);
                mds_unpack_body(body);
                memcpy(rootfid, &body->fid1, sizeof(*rootfid));
                *last_committed = req->rq_repmsg->last_committed;
                *last_rcvd = req->rq_repmsg->last_rcvd;
                *last_xid = body->last_xid;

                CDEBUG(D_NET, "root ino=%ld, last_committed=%Lu, last_rcvd=%Lu,"
                       " last_xid=%d\n",
                       (unsigned long)rootfid->id,
                       (unsigned long long)*last_committed,
                       (unsigned long long)*last_rcvd,
                       body->last_xid);
        }

        EXIT;
 out:
        ptlrpc_free_req(req); 
        return rc;
}


int mdc_getattr(struct lustre_handle *conn,
                obd_id ino, int type, unsigned long valid, size_t ea_size,
                struct ptlrpc_request **request)
{
        struct ptlrpc_client *cl;
        struct ptlrpc_connection *connection;
        struct lustre_handle *rconn;
        struct ptlrpc_request *req;
        struct mds_body *body;
        int rc, size[2] = {sizeof(*body), 0}, bufcount = 1;
        ENTRY;

        mdc_con2cl(conn, &cl, &connection, &rconn);
        req = ptlrpc_prep_req2(cl, connection, rconn,
                               MDS_GETATTR, 1, size, NULL);
        if (!req)
                GOTO(out, rc = -ENOMEM);

        body = lustre_msg_buf(req->rq_reqmsg, 0);
        ll_ino2fid(&body->fid1, ino, 0, type);
        body->valid = valid;

        if (S_ISREG(type)) {
                struct mdc_obd *mdc = &class_conn2obd(conn)->u.mdc;
                bufcount = 2;
                size[1] = mdc->mdc_max_mdsize;
        } else if (valid & OBD_MD_LINKNAME) {
                bufcount = 2;
                size[1] = ea_size;
        }
        req->rq_replen = lustre_msg_size(bufcount, size);

        rc = ptlrpc_queue_wait(req);
        rc = ptlrpc_check_status(req, rc);

        if (!rc) {
                body = lustre_msg_buf(req->rq_repmsg, 0);
                mds_unpack_body(body);
                CDEBUG(D_NET, "mode: %o\n", body->mode);
        }

        EXIT;
 out:
        *request = req;
        return rc;
}

static int mdc_lock_callback(struct lustre_handle *lockh,
                             struct ldlm_lock_desc *desc, void *data,
                             int data_len, struct ptlrpc_request **req)
{
        int rc;
        struct inode *inode = data;
        ENTRY;

        if (desc == NULL) {
                /* Completion AST.  Do nothing. */
                RETURN(0);
        }

        if (data_len != sizeof(*inode)) {
                CERROR("data_len should be %d, but is %d\n", sizeof(*inode),
                       data_len);
                LBUG();
        }

        /* FIXME: do something better than throwing away everything */
        if (inode == NULL)
                LBUG();
        if (S_ISDIR(inode->i_mode)) {
                CDEBUG(D_INODE, "invalidating inode %ld\n", inode->i_ino);
                invalidate_inode_pages(inode);
        }

        rc = ldlm_cli_cancel(lockh);
        if (rc < 0) {
                CERROR("ldlm_cli_cancel: %d\n", rc);
                LBUG();
        }
        RETURN(0);
}

int mdc_enqueue(struct lustre_handle *conn, int lock_type,
                struct lookup_intent *it, int lock_mode, struct inode *dir,
                struct dentry *de, struct lustre_handle *lockh, __u64 id,
                char *tgt, int tgtlen, void *data, int datalen)
{
        struct ptlrpc_request *req;
        struct obd_device *obddev = class_conn2obd(conn);
        struct ptlrpc_client *cl;
        struct ptlrpc_connection *connection;
        struct lustre_handle *rconn;
        __u64 res_id[RES_NAME_SIZE] = {dir->i_ino};
        int size[5] = {sizeof(struct ldlm_request), sizeof(struct ldlm_intent)};
        int rc, flags;
        int repsize[3] = {sizeof(struct ldlm_reply), 
                          sizeof(struct mds_body),
                          obddev->u.mdc.mdc_max_mdsize};
        struct ldlm_reply *dlm_rep;
        struct ldlm_intent *lit;
        ENTRY;

        LDLM_DEBUG_NOLOCK("mdsintent %d dir %ld", it->it_op, dir->i_ino);

        switch (it->it_op) { 
        case IT_MKDIR:
                it->it_mode = (it->it_mode | S_IFDIR) & ~current->fs->umask; 
                break;
        case (IT_CREAT|IT_OPEN):
        case IT_CREAT:
        case IT_MKNOD:
                it->it_mode = (it->it_mode | S_IFREG) & ~current->fs->umask; 
                break;
        case IT_SYMLINK:
                it->it_mode = (it->it_mode | S_IFLNK) & ~current->fs->umask; 
                break;
        }

        mdc_con2dlmcl(conn, &cl, &connection, &rconn);
        if (it->it_op & (IT_MKDIR | IT_CREAT | IT_SYMLINK | IT_MKNOD)) {
                size[2] = sizeof(struct mds_rec_create);
                size[3] = de->d_name.len + 1;
                size[4] = tgtlen + 1;
                req = ptlrpc_prep_req2(cl, connection, rconn,
                                       LDLM_ENQUEUE, 5, size, NULL);
                if (!req)
                        RETURN(-ENOMEM);

                /* pack the intent */
                lit = lustre_msg_buf(req->rq_reqmsg, 1);
                lit->opc = NTOH__u64((__u64)it->it_op);

                /* pack the intended request */
                mds_create_pack(req, 2, dir, it->it_mode, id, current->fsuid,
                                current->fsgid, CURRENT_TIME, de->d_name.name,
                                de->d_name.len, tgt, tgtlen);
                req->rq_replen = lustre_msg_size(3, repsize);
        } else if (it->it_op == IT_RENAME2) {
                struct dentry *old_de = it->it_data;

                size[2] = sizeof(struct mds_rec_rename);
                size[3] = old_de->d_name.len + 1;
                size[4] = de->d_name.len + 1;
                req = ptlrpc_prep_req2(cl, connection, rconn,
                                       LDLM_ENQUEUE, 5, size, NULL);
                if (!req)
                        RETURN(-ENOMEM);

                /* pack the intent */
                lit = lustre_msg_buf(req->rq_reqmsg, 1);
                lit->opc = NTOH__u64((__u64)it->it_op);

                /* pack the intended request */
                mds_rename_pack(req, 2, old_de->d_parent->d_inode, dir,
                                old_de->d_name.name, old_de->d_name.len,
                                de->d_name.name, de->d_name.len);
                req->rq_replen = lustre_msg_size(1, repsize);
        } else if (it->it_op == IT_UNLINK || it->it_op == IT_RMDIR) {
                size[2] = sizeof(struct mds_rec_unlink);
                size[3] = de->d_name.len + 1;
                req = ptlrpc_prep_req2(cl, connection, rconn,
                                       LDLM_ENQUEUE, 4, size, NULL);
                if (!req)
                        RETURN(-ENOMEM);

                /* pack the intent */
                lit = lustre_msg_buf(req->rq_reqmsg, 1);
                lit->opc = NTOH__u64((__u64)it->it_op);

                /* pack the intended request */
                mds_unlink_pack(req, 2, dir, NULL, de->d_name.name, 
                                de->d_name.len);

                req->rq_replen = lustre_msg_size(3, repsize);
        } else if (it->it_op == IT_GETATTR || it->it_op == IT_RENAME ||
                   it->it_op == IT_OPEN || it->it_op == IT_SETATTR ||
                   it->it_op == IT_LOOKUP) {
                size[2] = sizeof(struct mds_body);
                size[3] = de->d_name.len + 1;

                req = ptlrpc_prep_req2(cl, connection, rconn,
                                       LDLM_ENQUEUE, 4, size, NULL);
                if (!req)
                        RETURN(-ENOMEM);

                /* pack the intent */
                lit = lustre_msg_buf(req->rq_reqmsg, 1);
                lit->opc = NTOH__u64((__u64)it->it_op);

                /* pack the intended request */
                mds_getattr_pack(req, 2, dir, de->d_name.name, de->d_name.len);

                /* get ready for the reply */
                req->rq_replen = lustre_msg_size(3, repsize);
        } else if (it->it_op == IT_READDIR) {
                req = ptlrpc_prep_req2(cl, connection, rconn,
                                       LDLM_ENQUEUE, 1, size, NULL);
                if (!req)
                        RETURN(-ENOMEM);

                /* get ready for the reply */
                req->rq_replen = lustre_msg_size(1, repsize);
        } else {
                LBUG();
                RETURN(-1);
        }
#warning FIXME: the data here needs to be different if a lock was granted for a different inode
        rc = ldlm_cli_enqueue(cl, connection, NULL, req,
                              obddev->obd_namespace, NULL, res_id, lock_type,
                              NULL, 0, lock_mode, &flags,
                              (void *)mdc_lock_callback, data, datalen, lockh);
        if (rc == -ENOENT || rc == ELDLM_LOCK_ABORTED) {
                lock_mode = 0;
                memset(lockh, 0, sizeof(*lockh));
        } else if (rc != 0) {
                CERROR("ldlm_cli_enqueue: %d\n", rc);
                RETURN(rc);
        }

        dlm_rep = lustre_msg_buf(req->rq_repmsg, 0); 
        it->it_disposition = (int) dlm_rep->lock_policy_res1;
        it->it_status = (int) dlm_rep->lock_policy_res2;
        it->it_lock_mode = lock_mode;
        it->it_data = req;

        RETURN(0);
}

int mdc_open(struct lustre_handle *conn, obd_id ino, int type, int flags,
             struct lov_stripe_md *smd, __u64 cookie, __u64 *fh,
             struct ptlrpc_request **request)
{
        struct ptlrpc_client *cl;
        struct ptlrpc_connection *connection;
        struct lustre_handle *rconn;
        struct mds_body *body;
        int rc, size[2] = {sizeof(*body)}, bufcount = 1;
        struct ptlrpc_request *req;
        ENTRY;

        if (smd != NULL) {
                bufcount = 2;
                size[1] = smd->lmd_size;
        }

        mdc_con2cl(conn, &cl, &connection, &rconn);
        req = ptlrpc_prep_req2(cl, connection, rconn,
                               MDS_OPEN, bufcount, size, NULL);
        if (!req)
                GOTO(out, rc = -ENOMEM);

        req->rq_flags |= PTL_RPC_FL_REPLAY;
        body = lustre_msg_buf(req->rq_reqmsg, 0);

        ll_ino2fid(&body->fid1, ino, 0, type);
        body->flags = HTON__u32(flags);
        body->extra = cookie;

        if (smd != NULL)
                memcpy(lustre_msg_buf(req->rq_reqmsg, 1), smd, smd->lmd_size);

        req->rq_replen = lustre_msg_size(1, size);

        rc = ptlrpc_queue_wait(req);
        rc = ptlrpc_check_status(req, rc);
        if (!rc) {
                body = lustre_msg_buf(req->rq_repmsg, 0);
                mds_unpack_body(body);
                *fh = body->extra;
        }

        EXIT;
 out:
        *request = req;
        return rc;
}

int mdc_close(struct lustre_handle *conn, 
              obd_id ino, int type, __u64 fh, struct ptlrpc_request **request)
{
        struct ptlrpc_client *cl;
        struct ptlrpc_connection *connection;
        struct lustre_handle *rconn;
        struct mds_body *body;
        int rc, size = sizeof(*body);
        struct ptlrpc_request *req;

        mdc_con2cl(conn, &cl, &connection, &rconn);
        req = ptlrpc_prep_req2(cl, connection, rconn,
                               MDS_CLOSE, 1, &size, NULL);
        if (!req)
                GOTO(out, rc = -ENOMEM);

        body = lustre_msg_buf(req->rq_reqmsg, 0);
        ll_ino2fid(&body->fid1, ino, 0, type);
        body->extra = fh;

        req->rq_replen = lustre_msg_size(0, NULL);

        rc = ptlrpc_queue_wait(req);
        rc = ptlrpc_check_status(req, rc);

        EXIT;
 out:
        *request = req;
        return rc;
}

int mdc_readpage(struct lustre_handle *conn, obd_id ino, int type, __u64 offset,
                 char *addr, struct ptlrpc_request **request)
{
        struct ptlrpc_client *cl;
        struct ptlrpc_connection *connection;
        struct lustre_handle *rconn;
        struct ptlrpc_request *req = NULL;
        struct ptlrpc_bulk_desc *desc = NULL;
        struct ptlrpc_bulk_page *bulk = NULL;
        struct mds_body *body;
        int rc, size = sizeof(*body);
        ENTRY;

        CDEBUG(D_INODE, "inode: %ld\n", (long)ino);

        mdc_con2cl(conn, &cl, &connection, &rconn);
        desc = ptlrpc_prep_bulk(connection);
        if (desc == NULL)
                GOTO(out, rc = -ENOMEM);

        req = ptlrpc_prep_req2(cl, connection, rconn,
                               MDS_READPAGE, 1, &size, NULL);
        if (!req)
                GOTO(out2, rc = -ENOMEM);

        bulk = ptlrpc_prep_bulk_page(desc);
        bulk->b_buflen = PAGE_SIZE;
        bulk->b_buf = addr;
        bulk->b_xid = req->rq_xid;
        desc->b_portal = MDS_BULK_PORTAL;

        rc = ptlrpc_register_bulk(desc);
        if (rc) {
                CERROR("couldn't setup bulk sink: error %d.\n", rc);
                GOTO(out2, rc);
        }

        body = lustre_msg_buf(req->rq_reqmsg, 0);
        body->fid1.id = ino;
        body->fid1.f_type = type;
        body->size = offset;

        req->rq_replen = lustre_msg_size(1, &size);
        rc = ptlrpc_queue_wait(req);
        rc = ptlrpc_check_status(req, rc);
        if (rc) {
                ptlrpc_abort_bulk(desc);
                GOTO(out2, rc);
        } else { 
                body = lustre_msg_buf(req->rq_repmsg, 0);
                mds_unpack_body(body);
        }

        EXIT;
 out2:
        ptlrpc_free_bulk(desc);
 out:
        *request = req;
        return rc;
}

int mdc_statfs(struct lustre_handle *conn, struct statfs *sfs,
               struct ptlrpc_request **request)
{
        struct ptlrpc_client *cl;
        struct ptlrpc_connection *connection;
        struct lustre_handle *rconn;
        struct obd_statfs *osfs;
        struct ptlrpc_request *req;
        int rc, size = sizeof(*osfs);
        ENTRY;

        mdc_con2cl(conn, &cl, &connection, &rconn);
        req = ptlrpc_prep_req2(cl, connection, rconn,
                               MDS_STATFS, 0, NULL, NULL);
        if (!req)
                GOTO(out, rc = -ENOMEM);
        req->rq_replen = lustre_msg_size(1, &size);

        rc = ptlrpc_queue_wait(req);
        rc = ptlrpc_check_status(req, rc);

        if (rc)
                GOTO(out, rc);

        osfs = lustre_msg_buf(req->rq_repmsg, 0);
        obd_statfs_unpack(osfs, sfs);

        EXIT;
out:
        *request = req;

        return rc;
}

static int mdc_ioctl(long cmd, struct lustre_handle *conn, int len, void *karg,
                     void *uarg)
{
#if 0
        /* FIXME XXX : This should use the new ioc_data to pass args in */
        int err = 0;
        struct ptlrpc_client *cl;
        struct ptlrpc_connection *connection;
        struct lustre_handle *rconn;
        struct ptlrpc_request *request;

        ENTRY;

        if (_IOC_TYPE(cmd) != IOC_REQUEST_TYPE ||
            _IOC_NR(cmd) < IOC_REQUEST_MIN_NR  ||
            _IOC_NR(cmd) > IOC_REQUEST_MAX_NR ) {
                CDEBUG(D_IOCTL, "invalid ioctl ( type %d, nr %d, size %d )\n",
                       _IOC_TYPE(cmd), _IOC_NR(cmd), _IOC_SIZE(cmd));
                RETURN(-EINVAL);
        }

        ptlrpc_init_client(NULL, NULL, 
                           MDS_REQUEST_PORTAL, MDC_REPLY_PORTAL, &cl);

        mdc_con2cl(conn, &cl, &connection, &rconn);
        switch (cmd) {
        case IOC_REQUEST_GETATTR: {
                CERROR("-- getting attr for ino %lu\n", arg);
                err = mdc_getattr(&cl, connection, (obd_id)arg, S_IFDIR, ~0, 0,
                                  &request);
                CERROR("-- done err %d\n", err);

                GOTO(out, err);
        }

        case IOC_REQUEST_READPAGE: {
                char *buf;
                OBD_ALLOC(buf, PAGE_SIZE);
                if (!buf) {
                        err = -ENOMEM;
                        GOTO(out, err);
                }
                CERROR("-- readpage 0 for ino %lu\n", arg);
                err = mdc_readpage(&cl, connection, arg, S_IFDIR, 0, buf,
                                   &request);
                CERROR("-- done err %d\n", err);
                OBD_FREE(buf, PAGE_SIZE);

                GOTO(out, err);
        }

        case IOC_REQUEST_SETATTR: {
                struct inode inode;
                struct iattr iattr;

                inode.i_ino = arg;
                inode.i_generation = 0;
                iattr.ia_mode = 040777;
                iattr.ia_atime = 0;
                iattr.ia_valid = ATTR_MODE | ATTR_ATIME;

                err = mdc_setattr(&cl, connection, &inode, &iattr, &request);
                CERROR("-- done err %d\n", err);

                GOTO(out, err);
        }

        case IOC_REQUEST_CREATE: {
                struct inode inode;
                struct iattr iattr;

                inode.i_ino = arg;
                inode.i_generation = 0;
                iattr.ia_mode = 040777;
                iattr.ia_atime = 0;
                iattr.ia_valid = ATTR_MODE | ATTR_ATIME;

                err = mdc_create(&cl, connection, &inode,
                                 "foofile", strlen("foofile"),
                                 NULL, 0, 0100707, 47114711,
                                 11, 47, 0, NULL, &request);
                CERROR("-- done err %d\n", err);

                GOTO(out, err);
        }

        case IOC_REQUEST_OPEN: {
                __u64 fh, ino;
                copy_from_user(&ino, (__u64 *)arg, sizeof(ino));
                CERROR("-- opening ino %llu\n", (unsigned long long)ino);
                err = mdc_open(&cl, connection, ino, S_IFDIR, O_RDONLY, 4711,
                               &fh, &request);
                copy_to_user((__u64 *)arg, &fh, sizeof(fh));
                CERROR("-- done err %d (fh=%Lu)\n", err,
                       (unsigned long long)fh);

                GOTO(out, err);
        }

        case IOC_REQUEST_CLOSE: {
                CERROR("-- closing ino 2, filehandle %lu\n", arg);
                err = mdc_close(&cl, connection, 2, S_IFDIR, arg, &request);
                CERROR("-- done err %d\n", err);

                GOTO(out, err);
        }

        default:
                GOTO(out, err = -EINVAL);
        }

 out:
        ptlrpc_free_req(request);
        ptlrpc_put_connection(connection);
        ptlrpc_cleanup_client(&cl);

        RETURN(err);
#endif
        return 0;
}

static int mdc_setup(struct obd_device *obddev, obd_count len, void *buf)
{
        struct obd_ioctl_data* data = buf;
        struct mdc_obd *mdc = &obddev->u.mdc;
        char server_uuid[37];
        int rc;
        ENTRY;

        if (data->ioc_inllen1 < 1) {
                CERROR("osc setup requires a TARGET UUID\n");
                RETURN(-EINVAL);
        }

        if (data->ioc_inllen1 > 37) {
                CERROR("mdc UUID must be less than 38 characters\n");
                RETURN(-EINVAL);
        }

        if (data->ioc_inllen2 < 1) {
                CERROR("mdc setup requires a SERVER UUID\n");
               RETURN(-EINVAL);
        }

        if (data->ioc_inllen2 > 37) {
                CERROR("mdc UUID must be less than 38 characters\n");
                RETURN(-EINVAL);
        }

        memcpy(mdc->mdc_target_uuid, data->ioc_inlbuf1, data->ioc_inllen1);
        memcpy(server_uuid, data->ioc_inlbuf2, MIN(data->ioc_inllen2,
                                                   sizeof(server_uuid)));

        mdc->mdc_conn = ptlrpc_uuid_to_connection(server_uuid);
        if (!mdc->mdc_conn)
                RETURN(-ENOENT); 

        OBD_ALLOC(mdc->mdc_client, sizeof(*mdc->mdc_client));
        if (mdc->mdc_client == NULL)
                GOTO(out_conn, rc = -ENOMEM);

        OBD_ALLOC(mdc->mdc_ldlm_client, sizeof(*mdc->mdc_ldlm_client));
        if (mdc->mdc_ldlm_client == NULL)
                GOTO(out_client, rc = -ENOMEM);

        ptlrpc_init_client(NULL, NULL, MDS_REQUEST_PORTAL, MDC_REPLY_PORTAL,
                           mdc->mdc_client);
        ptlrpc_init_client(NULL, NULL, LDLM_REQUEST_PORTAL, LDLM_REPLY_PORTAL,
                           mdc->mdc_ldlm_client);
        mdc->mdc_client->cli_name = "mdc";
        mdc->mdc_ldlm_client->cli_name = "ldlm";
        mdc->mdc_max_mdsize = sizeof(struct lov_stripe_md);
        /* XXX get recovery hooked in here again */
        //ptlrpc_init_client(ptlrpc_connmgr, ll_recover,...

        ptlrpc_init_client(ptlrpc_connmgr, NULL,
                           MDS_REQUEST_PORTAL, MDC_REPLY_PORTAL,
                           mdc->mdc_client);

        MOD_INC_USE_COUNT;
        RETURN(0);

 out_client:
        OBD_FREE(mdc->mdc_client, sizeof(*mdc->mdc_client));
 out_conn:
        ptlrpc_put_connection(mdc->mdc_conn);
        return rc;
}

static int mdc_cleanup(struct obd_device * obddev)
{
        struct mdc_obd *mdc = &obddev->u.mdc;

        ptlrpc_cleanup_client(mdc->mdc_client);
        OBD_FREE(mdc->mdc_client, sizeof(*mdc->mdc_client));
        ptlrpc_cleanup_client(mdc->mdc_ldlm_client);
        OBD_FREE(mdc->mdc_ldlm_client, sizeof(*mdc->mdc_ldlm_client));
        ptlrpc_put_connection(mdc->mdc_conn);

        MOD_DEC_USE_COUNT;
        return 0;
}

static int mdc_connect(struct lustre_handle *conn, struct obd_device *obd)
{
        struct mdc_obd *mdc = &obd->u.mdc;
        struct ptlrpc_request *request;
        int rc, size = sizeof(mdc->mdc_target_uuid);
        char *tmp = mdc->mdc_target_uuid;

        ENTRY;

        obd->obd_namespace =
                ldlm_namespace_new("mdc", LDLM_NAMESPACE_CLIENT);
        if (obd->obd_namespace == NULL)
                RETURN(-ENOMEM);

        MOD_INC_USE_COUNT;
        rc = class_connect(conn, obd);
        if (rc) 
                RETURN(rc); 

        request = ptlrpc_prep_req(mdc->mdc_client, mdc->mdc_conn,
                                  MDS_CONNECT, 1, &size, &tmp);
        if (!request)
                GOTO(out_disco, -ENOMEM);

        request->rq_level = LUSTRE_CONN_NEW;
        request->rq_replen = lustre_msg_size(0, NULL);
        /* Sending our local connection info breaks for local connections
        request->rq_reqmsg->addr = conn->addr;
        request->rq_reqmsg->cookie = conn->cookie;
         */

        rc = ptlrpc_queue_wait(request);
        rc = ptlrpc_check_status(request, rc);
        if (rc)
                GOTO(out, rc);

        class_rconn2export(conn, (struct lustre_handle *)request->rq_repmsg);

        EXIT;
 out:
        ptlrpc_free_req(request);
 out_disco:
        if (rc) {
                class_disconnect(conn);
                MOD_DEC_USE_COUNT;
        }
        return rc;
}

static int mdc_disconnect(struct lustre_handle *conn)
{
        struct ptlrpc_client *cl;
        struct ptlrpc_connection *connection;
        struct lustre_handle *rconn;
        struct obd_device *obd = class_conn2obd(conn);
        struct ptlrpc_request *request;
        int rc;
        ENTRY;

        ldlm_namespace_free(obd->obd_namespace);
        mdc_con2cl(conn, &cl, &connection, &rconn);
        request = ptlrpc_prep_req2(cl, connection, rconn,
                                   MDS_DISCONNECT, 0, NULL, NULL);
        if (!request)
                RETURN(-ENOMEM);

        request->rq_replen = lustre_msg_size(0, NULL);

        rc = ptlrpc_queue_wait(request);
        if (rc) 
                GOTO(out, rc);
        rc = class_disconnect(conn);
        if (!rc)
                MOD_DEC_USE_COUNT;
 out:
        ptlrpc_free_req(request);
        return rc;
}

struct obd_ops mdc_obd_ops = {
        o_setup:   mdc_setup,
        o_cleanup: mdc_cleanup,
        o_connect: mdc_connect,
        o_disconnect: mdc_disconnect,
        o_iocontrol: mdc_ioctl
};

static int __init ptlrpc_request_init(void)
{
        return class_register_type(&mdc_obd_ops, LUSTRE_MDC_NAME);
}

static void __exit ptlrpc_request_exit(void)
{
        class_unregister_type(LUSTRE_MDC_NAME);
}

MODULE_AUTHOR("Cluster File Systems <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Metadata Client v1.0");
MODULE_LICENSE("GPL");

EXPORT_SYMBOL(mdc_getstatus);
EXPORT_SYMBOL(mdc_enqueue);
EXPORT_SYMBOL(mdc_getattr);
EXPORT_SYMBOL(mdc_statfs);
EXPORT_SYMBOL(mdc_create);
EXPORT_SYMBOL(mdc_unlink);
EXPORT_SYMBOL(mdc_rename);
EXPORT_SYMBOL(mdc_link);
EXPORT_SYMBOL(mdc_readpage);
EXPORT_SYMBOL(mdc_setattr);
EXPORT_SYMBOL(mdc_close);
EXPORT_SYMBOL(mdc_open);

module_init(ptlrpc_request_init);
module_exit(ptlrpc_request_exit);
