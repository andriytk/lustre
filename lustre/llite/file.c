/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  linux/fs/ext2/file.c
 *
 * This code is issued under the GNU General Public License.
 * See the file COPYING in this distribution
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/file.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext2 fs regular file handling primitives
 *
 *  64-bit file support on 64-bit platforms by Jakub Jelinek
 *      (jj@sunsite.ms.mff.cuni.cz)
 */

#define DEBUG_SUBSYSTEM S_LLITE

#include <linux/lustre_dlm.h>
#include <linux/lustre_lite.h>

int ll_inode_setattr(struct inode *inode, struct iattr *attr, int do_trunc);
extern int ll_setattr(struct dentry *de, struct iattr *attr);

static int ll_file_open(struct inode *inode, struct file *file)
{
        int rc;
        struct ptlrpc_request *req = NULL;
        struct ll_file_data *fd;
        struct obdo *oa = NULL;
        struct lov_stripe_md *lsm = NULL;
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct ll_inode_info *lli = ll_i2info(inode);
        ENTRY;

        if (file->private_data)
                LBUG();

        lsm = lli->lli_smd;

        /*  delayed create of object (intent created inode) */
        /*  XXX object needs to be cleaned up if mdc_open fails */
        /*  XXX error handling appropriate here? */
        if (lsm == NULL) {
                struct inode *inode = file->f_dentry->d_inode;

                down(&lli->lli_open_sem);
                /* Check to see if we lost the race */
                if (lli->lli_smd == NULL) {
                        oa = obdo_alloc();
                        if (!oa) {
                                up(&lli->lli_open_sem);
                                RETURN(-ENOMEM);
                        }
                        oa->o_mode = S_IFREG | 0600;
                        oa->o_easize = ll_mds_easize(inode->i_sb);
                        oa->o_id = inode->i_ino;
                        oa->o_valid = OBD_MD_FLID | OBD_MD_FLTYPE |
                                OBD_MD_FLMODE | OBD_MD_FLEASIZE;
                        rc = obd_create(ll_i2obdconn(inode), oa, &lli->lli_smd);

                        if (rc) {
                                obdo_free(oa);
                                up(&lli->lli_open_sem);
                                RETURN(rc);
                        }
                }
                if (lli->lli_smd->lsm_object_id == 0)
                        LBUG();
                up(&lli->lli_open_sem);
                lsm = lli->lli_smd;
        }

        fd = kmem_cache_alloc(ll_file_data_slab, SLAB_KERNEL);
        if (!fd)
                GOTO(out, rc = -ENOMEM);
        memset(fd, 0, sizeof(*fd));

        rc = mdc_open(&sbi->ll_mdc_conn, inode->i_ino, S_IFREG | inode->i_mode,
                      file->f_flags, lsm, (__u64)(unsigned long)file,
                      &fd->fd_mdshandle, &req);
        fd->fd_req = req;
        ptlrpc_req_finished(req);
        if (rc)
                GOTO(out_req, -abs(rc));
        if (!fd->fd_mdshandle) {
                CERROR("mdc_open didn't assign fd_mdshandle\n");
                /* XXX handle this how, abort or is it non-fatal? */
        }

        if (oa == NULL && (oa = obdo_alloc()) == NULL)
                GOTO(out_mdc, rc = -EINVAL);

        oa->o_id = lsm->lsm_object_id;
        oa->o_mode = S_IFREG;
        oa->o_valid = OBD_MD_FLID | OBD_MD_FLTYPE | OBD_MD_FLSIZE;
        rc = obd_open(ll_i2obdconn(inode), oa, lsm);
        obdo_free(oa);
        oa = NULL;

        if (rc)
                GOTO(out_mdc, rc = -abs(rc));

        file->private_data = fd;

        EXIT;

        return 0;
out_mdc:
        mdc_close(&sbi->ll_mdc_conn, inode->i_ino,
                  S_IFREG, fd->fd_mdshandle, &req);
out_req:
        ptlrpc_free_req(req);
//out_fd:
        obdo_free(oa);
        kmem_cache_free(ll_file_data_slab, fd);
        file->private_data = NULL;
out:
        return rc;
}

int ll_size_lock(struct inode *inode, struct lov_stripe_md *lsm, __u64 start,
                 int mode, struct lustre_handle **lockhs_p)
{
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct ldlm_extent extent;
        struct lustre_handle *lockhs = NULL;
        int rc, flags = 0, stripe_count;
        ENTRY;

        if (sbi->ll_flags & LL_SBI_NOLCK) {
                *lockhs_p = NULL;
                RETURN(0);
        }

        stripe_count = lsm->lsm_stripe_count;
        if (!stripe_count)
                stripe_count = 1;

        OBD_ALLOC(lockhs, stripe_count * sizeof(*lockhs));
        if (lockhs == NULL)
                RETURN(-ENOMEM);

        extent.start = start;
        extent.end = ~0;

        rc = obd_enqueue(&sbi->ll_osc_conn, lsm, NULL, LDLM_EXTENT, &extent,
                         sizeof(extent), mode, &flags, ll_lock_callback,
                         inode, sizeof(*inode), lockhs);
        if (rc != ELDLM_OK) {
                CERROR("lock enqueue: %d\n", rc);
                OBD_FREE(lockhs, stripe_count * sizeof(*lockhs));
        } else
                *lockhs_p = lockhs;
        RETURN(rc);
}

int ll_size_unlock(struct inode *inode, struct lov_stripe_md *lsm, int mode,
                   struct lustre_handle *lockhs)
{
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        int rc, stripe_count;
        ENTRY;

        if (sbi->ll_flags & LL_SBI_NOLCK)
                RETURN(0);

        if (lockhs == NULL) {
                LBUG();
                RETURN(-EINVAL);
        }

        rc = obd_cancel(&sbi->ll_osc_conn, lsm, mode, lockhs);
        if (rc != ELDLM_OK) {
                CERROR("lock cancel: %d\n", rc);
                LBUG();
        }

        stripe_count = lsm->lsm_stripe_count;
        if (!stripe_count)
                stripe_count = 1;

        OBD_FREE(lockhs, stripe_count * sizeof(*lockhs));
        RETURN(rc);
}

int ll_file_size(struct inode *inode, struct lov_stripe_md *lsm)
{
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct lustre_handle *lockhs;
        struct obdo oa;
        int err, rc;
        ENTRY;

        LASSERT(lsm);
        LASSERT(sbi);

        rc = ll_size_lock(inode, lsm, 0, LCK_PR, &lockhs);
        if (rc != ELDLM_OK) {
                CERROR("lock enqueue: %d\n", rc);
                RETURN(rc);
        }

        oa.o_id = lsm->lsm_object_id;
        oa.o_mode = S_IFREG;
        oa.o_valid = OBD_MD_FLID|OBD_MD_FLTYPE|OBD_MD_FLSIZE|OBD_MD_FLBLOCKS;
        rc = obd_getattr(&sbi->ll_osc_conn, &oa, lsm);
        if (!rc)
                obdo_to_inode(inode, &oa,
                              oa.o_valid & ~(OBD_MD_FLTYPE | OBD_MD_FLMODE));

        err = ll_size_unlock(inode, lsm, LCK_PR, lockhs);
        if (err != ELDLM_OK) {
                CERROR("lock cancel: %d\n", err);
                LBUG();
        }
        RETURN(rc);
}

static int ll_file_release(struct inode *inode, struct file *file)
{
        int rc;
        struct ptlrpc_request *req = NULL;
        struct ll_file_data *fd;
        struct obdo oa;
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct ll_inode_info *lli = ll_i2info(inode);

        ENTRY;

        fd = (struct ll_file_data *)file->private_data;
        if (!fd || !fd->fd_mdshandle) {
                LBUG();
                GOTO(out, rc = -EINVAL);
        }

        memset(&oa, 0, sizeof(oa));
        oa.o_id = lli->lli_smd->lsm_object_id;
        oa.o_mode = S_IFREG;
        oa.o_valid = OBD_MD_FLTYPE | OBD_MD_FLID;
        rc = obd_close(ll_i2obdconn(inode), &oa, lli->lli_smd);
        if (rc)
                GOTO(out_fd, abs(rc));

        /* If this fails and we goto out_fd, the file size on the MDS is out of
         * date.  Is that a big deal? */
        if (file->f_mode & FMODE_WRITE) {
                struct lustre_handle *lockhs;

                rc = ll_size_lock(inode, lli->lli_smd, 0, LCK_PR, &lockhs);
                if (rc)
                        GOTO(out_fd, abs(rc));

                oa.o_id = lli->lli_smd->lsm_object_id;
                oa.o_mode = S_IFREG;
                oa.o_valid = OBD_MD_FLID | OBD_MD_FLTYPE | OBD_MD_FLSIZE |
                        OBD_MD_FLBLOCKS;
                rc = obd_getattr(&sbi->ll_osc_conn, &oa, lli->lli_smd);
                if (!rc) {
                        struct iattr attr;
                        attr.ia_valid = (ATTR_MTIME | ATTR_CTIME | ATTR_ATIME |
                                         ATTR_SIZE);
                        attr.ia_mtime = inode->i_mtime;
                        attr.ia_ctime = inode->i_ctime;
                        attr.ia_atime = inode->i_atime;
                        attr.ia_size = oa.o_size;

                        inode->i_blocks = oa.o_blocks;

                        /* XXX: this introduces a small race that we should
                         * evaluate */
                        rc = ll_inode_setattr(inode, &attr, 0);
                        if (rc) {
                                CERROR("failed - %d.\n", rc);
                                rc = -EIO; /* XXX - GOTO(out)? -phil */
                        }
                }
                rc = ll_size_unlock(inode, lli->lli_smd, LCK_PR, lockhs);
                if (rc) {
                        CERROR("lock cancel: %d\n", rc);
                        LBUG();
                }
        }

        rc = mdc_close(&sbi->ll_mdc_conn, inode->i_ino,
                       S_IFREG, fd->fd_mdshandle, &req);
        ptlrpc_req_finished(req);
        if (rc) {
                if (rc > 0)
                        rc = -rc;
                GOTO(out, rc);
        }
        ptlrpc_req_finished(fd->fd_req);

        rc = obd_cancel_unused(ll_i2obdconn(inode), lli->lli_smd, 0);
        if (rc)
                CERROR("obd_cancel_unused: %d\n", rc);

        EXIT;

out_fd:
        kmem_cache_free(ll_file_data_slab, fd);
        file->private_data = NULL;
out:
        return rc;
}


static inline void ll_remove_suid(struct inode *inode)
{
        unsigned int mode;

        /* set S_IGID if S_IXGRP is set, and always set S_ISUID */
        mode = (inode->i_mode & S_IXGRP)*(S_ISGID/S_IXGRP) | S_ISUID;

        /* was any of the uid bits set? */
        mode &= inode->i_mode;
        if (mode && !capable(CAP_FSETID)) {
                inode->i_mode &= ~mode;
                // XXX careful here - we cannot change the size
        }
}

static void ll_update_atime(struct inode *inode)
{
        struct iattr attr;

        attr.ia_atime = CURRENT_TIME;
        attr.ia_valid = ATTR_ATIME;

        if (inode->i_atime == attr.ia_atime) return;
        if (IS_RDONLY(inode)) return;
        if (IS_NOATIME(inode)) return;

        /* ll_inode_setattr() sets inode->i_atime from attr.ia_atime */
        ll_inode_setattr(inode, &attr, 0);
}

int ll_lock_callback(struct ldlm_lock *lock, struct ldlm_lock_desc *new,
                     void *data, __u32 data_len, int flag)
{
        struct inode *inode = data;
        struct lustre_handle lockh;
        int rc;
        ENTRY;

        if (data_len != sizeof(struct inode))
                LBUG();

        if (inode == NULL)
                LBUG();

        switch (flag) {
        case LDLM_CB_BLOCKING:
                ldlm_lock2handle(lock, &lockh);
                rc = ldlm_cli_cancel(&lockh);
                if (rc != ELDLM_OK)
                        CERROR("ldlm_cli_cancel failed: %d\n", rc);
                break;
        case LDLM_CB_CANCELING:
                down(&inode->i_sem);
                CDEBUG(D_INODE, "invalidating obdo/inode %ld\n", inode->i_ino);
                /* FIXME: do something better than throwing away everything */
                invalidate_inode_pages(inode);
                up(&inode->i_sem);
                break;
        default:
                LBUG();
        }

        RETURN(0);
}

static ssize_t ll_file_read(struct file *filp, char *buf, size_t count,
                            loff_t *ppos)
{
        struct ll_file_data *fd = (struct ll_file_data *)filp->private_data;
        struct inode *inode = filp->f_dentry->d_inode;
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct ldlm_extent extent;
        struct lustre_handle *lockhs = NULL;
        struct lov_stripe_md *lsm = ll_i2info(inode)->lli_smd;
        int flags = 0;
        ldlm_error_t err;
        ssize_t retval;
        ENTRY;

        if (!(fd->fd_flags & LL_FILE_IGNORE_LOCK) ||
            sbi->ll_flags & LL_SBI_NOLCK) {
                OBD_ALLOC(lockhs, lsm->lsm_stripe_count * sizeof(*lockhs));
                if (!lockhs)
                        RETURN(-ENOMEM);

                extent.start = *ppos;
                extent.end = *ppos + count;
                CDEBUG(D_INFO, "Locking inode %ld, start "LPU64" end "LPU64"\n",
                       inode->i_ino, extent.start, extent.end);

                err = obd_enqueue(&sbi->ll_osc_conn, lsm, NULL, LDLM_EXTENT,
                                  &extent, sizeof(extent), LCK_PR, &flags,
                                  ll_lock_callback, inode, sizeof(*inode),
                                  lockhs);
                if (err != ELDLM_OK) {
                        OBD_FREE(lockhs, lsm->lsm_stripe_count*sizeof(*lockhs));
                        CERROR("lock enqueue: err: %d\n", err);
                        RETURN(err);
                }
        }

        CDEBUG(D_INFO, "Reading inode %ld, %d bytes, offset %Ld\n",
               inode->i_ino, count, *ppos);
        retval = generic_file_read(filp, buf, count, ppos);

        if (retval > 0)
                ll_update_atime(inode);

        if (!(fd->fd_flags & LL_FILE_IGNORE_LOCK) ||
            sbi->ll_flags & LL_SBI_NOLCK) {
                err = obd_cancel(&sbi->ll_osc_conn, lsm, LCK_PR, lockhs);
                if (err != ELDLM_OK) {
                        CERROR("lock cancel: err: %d\n", err);
                        retval = err;
                }
        }

        if (lockhs)
                OBD_FREE(lockhs, lsm->lsm_stripe_count * sizeof(*lockhs));
        RETURN(retval);
}

/*
 * Write to a file (through the page cache).
 */
static ssize_t
ll_file_write(struct file *file, const char *buf, size_t count, loff_t *ppos)
{
        struct ll_file_data *fd = (struct ll_file_data *)file->private_data;
        struct inode *inode = file->f_dentry->d_inode;
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct ldlm_extent extent;
        struct lustre_handle *lockhs = NULL, *eof_lockhs = NULL;
        struct lov_stripe_md *lsm = ll_i2info(inode)->lli_smd;
        int flags = 0;
        ldlm_error_t err;
        ssize_t retval;
        ENTRY;

        if (!S_ISBLK(inode->i_mode) && file->f_flags & O_APPEND) {
                struct obdo oa;
                err = ll_size_lock(inode, lsm, 0, LCK_PW, &eof_lockhs);
                if (err)
                        RETURN(err);

                oa.o_id = lsm->lsm_object_id;
                oa.o_mode = inode->i_mode;
                oa.o_valid = OBD_MD_FLID | OBD_MD_FLTYPE | OBD_MD_FLSIZE |
                        OBD_MD_FLBLOCKS;
                retval = obd_getattr(&sbi->ll_osc_conn, &oa, lsm);
                if (retval)
                        GOTO(out_eof, retval);

                *ppos = oa.o_size;
                obdo_to_inode(inode, &oa, oa.o_valid);
        }

        if (!(fd->fd_flags & LL_FILE_IGNORE_LOCK) ||
            sbi->ll_flags & LL_SBI_NOLCK) {
                OBD_ALLOC(lockhs, lsm->lsm_stripe_count * sizeof(*lockhs));
                if (!lockhs)
                        GOTO(out_eof, retval = -ENOMEM);
                extent.start = *ppos;
                extent.end = *ppos + count;
                CDEBUG(D_INFO, "Locking inode %ld, start "LPU64" end "LPU64"\n",
                       inode->i_ino, extent.start, extent.end);

                err = obd_enqueue(&sbi->ll_osc_conn, lsm, NULL, LDLM_EXTENT,
                                  &extent, sizeof(extent), LCK_PW, &flags,
                                  ll_lock_callback, inode, sizeof(*inode),
                                  lockhs);
                if (err != ELDLM_OK) {
                        CERROR("lock enqueue: err: %d\n", err);
                        GOTO(out_free, retval = err);
                }
        }

        CDEBUG(D_INFO, "Writing inode %ld, %ld bytes, offset "LPD64"\n",
               inode->i_ino, (long)count, *ppos);

        retval = generic_file_write(file, buf, count, ppos);

        if (!(fd->fd_flags & LL_FILE_IGNORE_LOCK) ||
            sbi->ll_flags & LL_SBI_NOLCK) {
                err = obd_cancel(&sbi->ll_osc_conn, lsm, LCK_PW, lockhs);
                if (err != ELDLM_OK) {
                        CERROR("lock cancel: err: %d\n", err);
                        GOTO(out_free, retval = err);
                }
        }

        EXIT;
 out_free:
        if (lockhs)
                OBD_FREE(lockhs, lsm->lsm_stripe_count * sizeof(*lockhs));

 out_eof:
        if (!S_ISBLK(inode->i_mode) && file->f_flags & O_APPEND) {
                err = ll_size_unlock(inode, lsm, LCK_PW, eof_lockhs);
                if (err && !retval)
                        retval = err;
        }

        return retval;
}

int ll_file_ioctl(struct inode *inode, struct file *file, unsigned int cmd,
                  unsigned long arg)
{
        struct ll_file_data *fd = (struct ll_file_data *)file->private_data;
        int flags;

        switch(cmd) {
        case LL_IOC_GETFLAGS:
                /* Get the current value of the file flags */
                return put_user(fd->fd_flags, (int *)arg);
        case LL_IOC_SETFLAGS:
        case LL_IOC_CLRFLAGS:
                /* Set or clear specific file flags */
                /* XXX This probably needs checks to ensure the flags are
                 *     not abused, and to handle any flag side effects.
                 */
                if (get_user(flags, (int *) arg))
                        return -EFAULT;

                if (cmd == LL_IOC_SETFLAGS)
                        fd->fd_flags |= flags;
                else
                        fd->fd_flags &= ~flags;
                return 0;

        /* We need to special case any other ioctls we want to handle,
         * to send them to the MDS/OST as appropriate and to properly
         * network encode the arg field.
        case EXT2_IOC_GETFLAGS:
        case EXT2_IOC_SETFLAGS:
        case EXT2_IOC_GETVERSION_OLD:
        case EXT2_IOC_GETVERSION_NEW:
        case EXT2_IOC_SETVERSION_OLD:
        case EXT2_IOC_SETVERSION_NEW:
        */
        default:
                return -ENOTTY;
        }
}

loff_t ll_file_seek(struct file *file, loff_t offset, int origin)
{
        struct inode *inode = file->f_dentry->d_inode;
        long long retval;
        ENTRY;

        switch (origin) {
        case 2: {
                struct ll_inode_info *lli = ll_i2info(inode);

                retval = ll_file_size(inode, lli->lli_smd);
                if (retval)
                        RETURN(retval);

                offset += inode->i_size;
                break;
        }
        case 1:
                offset += file->f_pos;
        }
        retval = -EINVAL;
        if (offset >= 0 && offset <= inode->i_sb->s_maxbytes) {
                if (offset != file->f_pos) {
                        file->f_pos = offset;
                        file->f_reada = 0;
                        file->f_version = ++event;
                }
                retval = offset;
        }
        RETURN(retval);
}

/* XXX this does not need to do anything for data, it _does_ need to
   call setattr */
int ll_fsync(struct file *file, struct dentry *dentry, int data)
{
        return 0;
}

static int ll_inode_revalidate(struct dentry *dentry)
{
        struct inode *inode = dentry->d_inode;
        ENTRY;

        if (!inode)
                RETURN(0);

        RETURN(ll_file_size(inode, ll_i2info(inode)->lli_smd));
}

struct file_operations ll_file_operations = {
        read:           ll_file_read,
        write:          ll_file_write,
        ioctl:          ll_file_ioctl,
        open:           ll_file_open,
        release:        ll_file_release,
        mmap:           generic_file_mmap,
        llseek:         ll_file_seek,
        fsync:          NULL
};

struct inode_operations ll_file_inode_operations = {
        truncate:   ll_truncate,
        setattr:    ll_setattr,
        revalidate: ll_inode_revalidate
};
