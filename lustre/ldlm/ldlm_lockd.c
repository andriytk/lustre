/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2002 Cluster File Systems, Inc.
 *
 * This code is issued under the GNU General Public License.
 * See the file COPYING in this distribution
 *
 * by Cluster File Systems, Inc.
 */

#define EXPORT_SYMTAB
#define DEBUG_SUBSYSTEM S_LDLM

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/lustre_dlm.h>

extern kmem_cache_t *ldlm_resource_slab;
extern kmem_cache_t *ldlm_lock_slab;
extern int (*mds_reint_p)(int offset, struct ptlrpc_request *req);
extern int (*mds_getattr_name_p)(int offset, struct ptlrpc_request *req);

static int ldlm_handle_enqueue(struct ptlrpc_request *req)
{
        struct obd_device *obddev = req->rq_export->exp_obd;
        struct ldlm_reply *dlm_rep;
        struct ldlm_request *dlm_req;
        int rc, size = sizeof(*dlm_rep), cookielen = 0;
        __u32 flags;
        ldlm_error_t err;
        struct ldlm_lock *lock = NULL;
        void *cookie = NULL;
        ENTRY;

        LDLM_DEBUG_NOLOCK("server-side enqueue handler START");

        dlm_req = lustre_msg_buf(req->rq_reqmsg, 0);
        if (dlm_req->lock_desc.l_resource.lr_type == LDLM_MDSINTENT) {
                /* In this case, the reply buffer is allocated deep in
                 * local_lock_enqueue by the policy function. */
                cookie = req;
                cookielen = sizeof(*req);
        } else {
                rc = lustre_pack_msg(1, &size, NULL, &req->rq_replen,
                                     &req->rq_repmsg);
                if (rc) {
                        CERROR("out of memory\n");
                        RETURN(-ENOMEM);
                }
                if (dlm_req->lock_desc.l_resource.lr_type == LDLM_EXTENT) {
                        cookie = &dlm_req->lock_desc.l_extent;
                        cookielen = sizeof(struct ldlm_extent);
                }
        }

        lock = ldlm_lock_create(obddev->obd_namespace,
                                &dlm_req->lock_handle2,
                                dlm_req->lock_desc.l_resource.lr_name,
                                dlm_req->lock_desc.l_resource.lr_type,
                                dlm_req->lock_desc.l_req_mode, NULL, 0);
        if (!lock)
                GOTO(out, err = -ENOMEM);

        memcpy(&lock->l_remote_handle, &dlm_req->lock_handle1,
               sizeof(lock->l_remote_handle));
        LDLM_DEBUG(lock, "server-side enqueue handler, new lock created");

        flags = dlm_req->lock_flags;
        err = ldlm_lock_enqueue(lock, cookie, cookielen, &flags,
                                ldlm_server_ast, ldlm_server_ast);
        if (err != ELDLM_OK)
                GOTO(out, err);

        dlm_rep = lustre_msg_buf(req->rq_repmsg, 0);
        dlm_rep->lock_flags = flags;

        ldlm_lock2handle(lock, &dlm_rep->lock_handle);
        if (dlm_req->lock_desc.l_resource.lr_type == LDLM_EXTENT)
                memcpy(&dlm_rep->lock_extent, &lock->l_extent,
                       sizeof(lock->l_extent));
        if (dlm_rep->lock_flags & LDLM_FL_LOCK_CHANGED)
                memcpy(dlm_rep->lock_resource_name, lock->l_resource->lr_name,
                       sizeof(dlm_rep->lock_resource_name));

        lock->l_connection = ptlrpc_connection_addref(req->rq_connection);
        EXIT;
 out:
        if (lock)
                LDLM_DEBUG(lock, "server-side enqueue handler, sending reply"
                           "(err=%d)", err);
        req->rq_status = err;

        if (ptlrpc_reply(req->rq_svc, req))
                LBUG();

        if (lock) {
                if (!err)
                        ldlm_reprocess_all(lock->l_resource);
                LDLM_LOCK_PUT(lock);
        }
        LDLM_DEBUG_NOLOCK("server-side enqueue handler END (lock %p)", lock);

        return 0;
}

static int ldlm_handle_convert(struct ptlrpc_request *req)
{
        struct ldlm_request *dlm_req;
        struct ldlm_reply *dlm_rep;
        struct ldlm_lock *lock;
        int rc, size = sizeof(*dlm_rep);
        ENTRY;

        rc = lustre_pack_msg(1, &size, NULL, &req->rq_replen, &req->rq_repmsg);
        if (rc) {
                CERROR("out of memory\n");
                RETURN(-ENOMEM);
        }
        dlm_req = lustre_msg_buf(req->rq_reqmsg, 0);
        dlm_rep = lustre_msg_buf(req->rq_repmsg, 0);
        dlm_rep->lock_flags = dlm_req->lock_flags;

        lock = ldlm_handle2lock(&dlm_req->lock_handle1);
        if (!lock) {
                req->rq_status = EINVAL;
        } else {
                LDLM_DEBUG(lock, "server-side convert handler START");
                ldlm_lock_convert(lock, dlm_req->lock_desc.l_req_mode,
                                  &dlm_rep->lock_flags);
                req->rq_status = 0;
        }
        if (ptlrpc_reply(req->rq_svc, req) != 0)
                LBUG();

        if (lock) {
                ldlm_reprocess_all(lock->l_resource);
                LDLM_DEBUG(lock, "server-side convert handler END");
                LDLM_LOCK_PUT(lock);
        } else
                LDLM_DEBUG_NOLOCK("server-side convert handler END");

        RETURN(0);
}

static int ldlm_handle_cancel(struct ptlrpc_request *req)
{
        struct ldlm_request *dlm_req;
        struct ldlm_lock *lock;
        int rc;
        ENTRY;

        rc = lustre_pack_msg(0, NULL, NULL, &req->rq_replen, &req->rq_repmsg);
        if (rc) {
                CERROR("out of memory\n");
                RETURN(-ENOMEM);
        }
        dlm_req = lustre_msg_buf(req->rq_reqmsg, 0);

        lock = ldlm_handle2lock(&dlm_req->lock_handle1);
        if (!lock) {
                req->rq_status = ESTALE;
        } else {
                LDLM_DEBUG(lock, "server-side cancel handler START");
                ldlm_lock_cancel(lock);
                req->rq_status = 0;
        }

        if (ptlrpc_reply(req->rq_svc, req) != 0)
                LBUG();

        if (lock) {
                ldlm_reprocess_all(lock->l_resource);
                LDLM_DEBUG(lock, "server-side cancel handler END");
                LDLM_LOCK_PUT(lock);
        } else
                LDLM_DEBUG_NOLOCK("server-side cancel handler END (lock %p)",
                                  lock);

        RETURN(0);
}

static int ldlm_handle_callback(struct ptlrpc_request *req)
{
        struct ldlm_request *dlm_req;
        struct ldlm_lock_desc *descp = NULL;
        struct ldlm_lock *lock;
        __u64 is_blocking_ast = 0;
        int rc;
        ENTRY;

        rc = lustre_pack_msg(0, NULL, NULL, &req->rq_replen, &req->rq_repmsg);
        if (rc) {
                CERROR("out of memory\n");
                RETURN(-ENOMEM);
        }
        dlm_req = lustre_msg_buf(req->rq_reqmsg, 0);

        /* We must send the reply first, so that the thread is free to handle
         * any requests made in common_callback() */
        rc = ptlrpc_reply(req->rq_svc, req);
        if (rc != 0)
                RETURN(rc);

        lock = ldlm_handle2lock(&dlm_req->lock_handle1);
        if (!lock) {
                CERROR("callback on lock %Lx - lock disappeared\n",
                       dlm_req->lock_handle1.addr);
                RETURN(0);
        }

        /* check if this is a blocking AST */
        if (dlm_req->lock_desc.l_req_mode !=
            dlm_req->lock_desc.l_granted_mode) {
                descp = &dlm_req->lock_desc;
                is_blocking_ast = 1;
        }

        LDLM_DEBUG(lock, "client %s callback handler START",
                   is_blocking_ast ? "blocked" : "completion");

        if (descp) {
                int do_ast;
                l_lock(&lock->l_resource->lr_namespace->ns_lock);
                lock->l_flags |= LDLM_FL_CBPENDING;
                do_ast = (!lock->l_readers && !lock->l_writers);
                l_unlock(&lock->l_resource->lr_namespace->ns_lock);

                if (do_ast) {
                        LDLM_DEBUG(lock, "already unused, calling "
                                   "callback (%p)", lock->l_blocking_ast);
                        if (lock->l_blocking_ast != NULL) {
                                struct lustre_handle lockh;
                                ldlm_lock2handle(lock, &lockh);
                                lock->l_blocking_ast(&lockh, descp,
                                                     lock->l_data,
                                                     lock->l_data_len);
                        }
                } else {
                        LDLM_DEBUG(lock, "Lock still has references, will be"
                                   " cancelled later");
                }
                LDLM_LOCK_PUT(lock);
        } else {
                struct list_head rpc_list = LIST_HEAD_INIT(rpc_list);

                l_lock(&lock->l_resource->lr_namespace->ns_lock);
                lock->l_req_mode = dlm_req->lock_desc.l_granted_mode;

                /* If we receive the completion AST before the actual enqueue
                 * returned, then we might need to switch resources. */
                if (memcmp(dlm_req->lock_desc.l_resource.lr_name,
                           lock->l_resource->lr_name,
                           sizeof(__u64) * RES_NAME_SIZE) != 0) {
                        ldlm_lock_change_resource(lock, dlm_req->lock_desc.l_resource.lr_name);
                        LDLM_DEBUG(lock, "completion AST, new resource");
                }
                lock->l_resource->lr_tmp = &rpc_list;
                ldlm_resource_unlink_lock(lock);
                ldlm_grant_lock(lock);
                /*  FIXME: we want any completion function, not just wake_up */
                wake_up(&lock->l_waitq);
                lock->l_resource->lr_tmp = NULL;
                l_unlock(&lock->l_resource->lr_namespace->ns_lock);
                LDLM_LOCK_PUT(lock);

                ldlm_run_ast_work(&rpc_list);
        }

        LDLM_DEBUG_NOLOCK("client %s callback handler END (lock %p)",
                          is_blocking_ast ? "blocked" : "completion", lock);
        RETURN(0);
}

static int ldlm_handle(struct ptlrpc_request *req)
{
        int rc;
        ENTRY;

        rc = lustre_unpack_msg(req->rq_reqmsg, req->rq_reqlen);
        if (rc) {
                CERROR("lustre_ldlm: Invalid request\n");
                GOTO(out, rc);
        }

        if (req->rq_reqmsg->type != PTL_RPC_MSG_REQUEST) {
                CERROR("lustre_ldlm: wrong packet type sent %d\n",
                       req->rq_reqmsg->type);
                GOTO(out, rc = -EINVAL);
        }

        if (!req->rq_export && req->rq_reqmsg->opc == LDLM_ENQUEUE) {
                CERROR("No export handle for enqueue request.\n");
                GOTO(out, rc = -ENOTCONN);
        }

        switch (req->rq_reqmsg->opc) {
        case LDLM_ENQUEUE:
                CDEBUG(D_INODE, "enqueue\n");
                OBD_FAIL_RETURN(OBD_FAIL_LDLM_ENQUEUE, 0);
                rc = ldlm_handle_enqueue(req);
                break;

        case LDLM_CONVERT:
                CDEBUG(D_INODE, "convert\n");
                OBD_FAIL_RETURN(OBD_FAIL_LDLM_CONVERT, 0);
                rc = ldlm_handle_convert(req);
                break;

        case LDLM_CANCEL:
                CDEBUG(D_INODE, "cancel\n");
                OBD_FAIL_RETURN(OBD_FAIL_LDLM_CANCEL, 0);
                rc = ldlm_handle_cancel(req);
                break;

        case LDLM_CALLBACK:
                CDEBUG(D_INODE, "callback\n");
                OBD_FAIL_RETURN(OBD_FAIL_LDLM_CALLBACK, 0);
                rc = ldlm_handle_callback(req);
                break;

        default:
                rc = ptlrpc_error(req->rq_svc, req);
                RETURN(rc);
        }

        EXIT;
out:
        if (rc)
                RETURN(ptlrpc_error(req->rq_svc, req));
        return 0;
}

static int ldlm_iocontrol(long cmd, struct lustre_handle *conn, int len,
                          void *karg, void *uarg)
{
        struct obd_device *obddev = class_conn2obd(conn);
        struct ptlrpc_connection *connection;
        int err;
        ENTRY;

        if (_IOC_TYPE(cmd) != IOC_LDLM_TYPE || _IOC_NR(cmd) < IOC_LDLM_MIN_NR ||
            _IOC_NR(cmd) > IOC_LDLM_MAX_NR) {
                CDEBUG(D_IOCTL, "invalid ioctl (type %ld, nr %ld, size %ld)\n",
                       _IOC_TYPE(cmd), _IOC_NR(cmd), _IOC_SIZE(cmd));
                RETURN(-EINVAL);
        }

        OBD_ALLOC(obddev->u.ldlm.ldlm_client,
                  sizeof(*obddev->u.ldlm.ldlm_client));
        ptlrpc_init_client(NULL, NULL,
                           LDLM_REQUEST_PORTAL, LDLM_REPLY_PORTAL,
                           obddev->u.ldlm.ldlm_client);
        connection = ptlrpc_uuid_to_connection("ldlm");
        if (!connection)
                CERROR("No LDLM UUID found: assuming ldlm is local.\n");

        switch (cmd) {
        case IOC_LDLM_TEST: {
                err = ldlm_test(obddev, connection);
                CERROR("-- done err %d\n", err);
                GOTO(out, err);
        }
        default:
                GOTO(out, err = -EINVAL);
        }

 out:
        if (connection)
                ptlrpc_put_connection(connection);
        OBD_FREE(obddev->u.ldlm.ldlm_client,
                 sizeof(*obddev->u.ldlm.ldlm_client));
        return err;
}

#define LDLM_NUM_THREADS        8

static int ldlm_setup(struct obd_device *obddev, obd_count len, void *buf)
{
        struct ldlm_obd *ldlm = &obddev->u.ldlm;
        int rc;
        int i;
        ENTRY;

        MOD_INC_USE_COUNT;
        ldlm->ldlm_service =
                ptlrpc_init_svc(64 * 1024, LDLM_REQUEST_PORTAL,
                                LDLM_REPLY_PORTAL, "self", ldlm_handle);
        if (!ldlm->ldlm_service) {
                LBUG();
                GOTO(out_dec, rc = -ENOMEM);
        }

        if (mds_reint_p == NULL)
                mds_reint_p = inter_module_get_request("mds_reint", "mds");
        if (IS_ERR(mds_reint_p)) {
                CERROR("MDSINTENT locks require the MDS module.\n");
                GOTO(out_dec, rc = -EINVAL);
        }
        if (mds_getattr_name_p == NULL)
                mds_getattr_name_p = inter_module_get_request
                        ("mds_getattr_name", "mds");
        if (IS_ERR(mds_getattr_name_p)) {
                CERROR("MDSINTENT locks require the MDS module.\n");
                GOTO(out_dec, rc = -EINVAL);
        }

        for (i = 0; i < LDLM_NUM_THREADS; i++) {
                rc = ptlrpc_start_thread(obddev, ldlm->ldlm_service,
                                         "lustre_dlm");
                if (rc) {
                        CERROR("cannot start LDLM thread #%d: rc %d\n", i, rc);
                        LBUG();
                        GOTO(out_thread, rc);
                }
        }

        RETURN(0);

out_thread:
        ptlrpc_stop_all_threads(ldlm->ldlm_service);
        ptlrpc_unregister_service(ldlm->ldlm_service);
out_dec:
        if (mds_reint_p != NULL)
                inter_module_put("mds_reint");
        if (mds_getattr_name_p != NULL)
                inter_module_put("mds_getattr_name");
        MOD_DEC_USE_COUNT;
        return rc;
}

static int ldlm_cleanup(struct obd_device *obddev)
{
        struct ldlm_obd *ldlm = &obddev->u.ldlm;
        ENTRY;

        ptlrpc_stop_all_threads(ldlm->ldlm_service);
        ptlrpc_unregister_service(ldlm->ldlm_service);

        if (mds_reint_p != NULL)
                inter_module_put("mds_reint");
        if (mds_getattr_name_p != NULL)
                inter_module_put("mds_getattr_name");

        MOD_DEC_USE_COUNT;
        RETURN(0);
}

struct obd_ops ldlm_obd_ops = {
        o_iocontrol:   ldlm_iocontrol,
        o_setup:       ldlm_setup,
        o_cleanup:     ldlm_cleanup,
        o_connect:     class_connect,
        o_disconnect:  class_disconnect
};


static int __init ldlm_init(void)
{
        int rc = class_register_type(&ldlm_obd_ops, OBD_LDLM_DEVICENAME);
        if (rc != 0)
                return rc;

        ldlm_resource_slab = kmem_cache_create("ldlm_resources",
                                               sizeof(struct ldlm_resource), 0,
                                               SLAB_HWCACHE_ALIGN, NULL, NULL);
        if (ldlm_resource_slab == NULL)
                return -ENOMEM;

        ldlm_lock_slab = kmem_cache_create("ldlm_locks",
                                           sizeof(struct ldlm_lock), 0,
                                           SLAB_HWCACHE_ALIGN, NULL, NULL);
        if (ldlm_lock_slab == NULL) {
                kmem_cache_destroy(ldlm_resource_slab);
                return -ENOMEM;
        }

        return 0;
}

static void __exit ldlm_exit(void)
{
        class_unregister_type(OBD_LDLM_DEVICENAME);
        if (kmem_cache_destroy(ldlm_resource_slab) != 0)
                CERROR("couldn't free ldlm resource slab\n");
        if (kmem_cache_destroy(ldlm_lock_slab) != 0)
                CERROR("couldn't free ldlm lock slab\n");
}

EXPORT_SYMBOL(ldlm_lockname);
EXPORT_SYMBOL(ldlm_typename);
EXPORT_SYMBOL(ldlm_handle2lock);
EXPORT_SYMBOL(ldlm_lock_match);
EXPORT_SYMBOL(ldlm_lock_addref);
EXPORT_SYMBOL(ldlm_lock_decref);
EXPORT_SYMBOL(ldlm_cli_convert);
EXPORT_SYMBOL(ldlm_cli_enqueue);
EXPORT_SYMBOL(ldlm_cli_cancel);
EXPORT_SYMBOL(ldlm_match_or_enqueue);
EXPORT_SYMBOL(ldlm_test);
EXPORT_SYMBOL(ldlm_lock_dump);
EXPORT_SYMBOL(ldlm_namespace_new);
EXPORT_SYMBOL(ldlm_namespace_free);

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Lock Management Module v0.1");
MODULE_LICENSE("GPL");

module_init(ldlm_init);
module_exit(ldlm_exit);
