/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  linux/fs/obdfilter/filter.c
 *
 * Copyright (C) 2001, 2002 Cluster File Systems, Inc.
 *
 * This code is issued under the GNU General Public License.
 * See the file COPYING in this distribution
 *
 * by Peter Braam <braam@clusterfs.com>
 * and Andreas Dilger <adilger@clusterfs.com>
 */

#define EXPORT_SYMTAB
#define DEBUG_SUBSYSTEM S_FILTER

#include <linux/module.h>
#include <linux/lustre_dlm.h>
#include <linux/obd_filter.h>
#include <linux/ext3_jbd.h>
#include <linux/quotaops.h>
#include <linux/init.h>

long filter_memory;

#define FILTER_ROOTINO 2

#define S_SHIFT 12
static char *obd_type_by_mode[S_IFMT >> S_SHIFT] = {
        [0]                     NULL,
        [S_IFREG >> S_SHIFT]    "R",
        [S_IFDIR >> S_SHIFT]    "D",
        [S_IFCHR >> S_SHIFT]    "C",
        [S_IFBLK >> S_SHIFT]    "B",
        [S_IFIFO >> S_SHIFT]    "F",
        [S_IFSOCK >> S_SHIFT]   "S",
        [S_IFLNK >> S_SHIFT]    "L"
};

static inline const char *obd_mode_to_type(int mode)
{
        return obd_type_by_mode[(mode & S_IFMT) >> S_SHIFT];
}

/* write the pathname into the string */
static int filter_id(char *buf, obd_id id, obd_mode mode)
{
        return sprintf(buf, "O/%s/%Ld", obd_mode_to_type(mode),
                       (unsigned long long)id);
}

static inline void f_dput(struct dentry *dentry)
{
        CDEBUG(D_INODE, "putting %s: %p, count = %d\n",
               dentry->d_name.name, dentry, atomic_read(&dentry->d_count) - 1);
        LASSERT(atomic_read(&dentry->d_count) > 0);
        dput(dentry);
}

/* setup the object store with correct subdirectories */
static int filter_prep(struct obd_device *obddev)
{
        struct obd_run_ctxt saved;
        struct filter_obd *filter = &obddev->u.filter;
        struct dentry *dentry;
        struct file *file;
        struct inode *inode;
        int rc = 0;
        char rootid[128];
        __u64 lastino = 2;
        int mode = 0;

        push_ctxt(&saved, &filter->fo_ctxt);
        dentry = simple_mkdir(current->fs->pwd, "O", 0700);
        CDEBUG(D_INODE, "got/created O: %p\n", dentry);
        if (IS_ERR(dentry)) {
                rc = PTR_ERR(dentry);
                CERROR("cannot open/create O: rc = %d\n", rc);
                GOTO(out, rc);
        }
        filter->fo_dentry_O = dentry;
        dentry = simple_mkdir(current->fs->pwd, "P", 0700);
        CDEBUG(D_INODE, "got/created P: %p\n", dentry);
        if (IS_ERR(dentry)) {
                rc = PTR_ERR(dentry);
                CERROR("cannot open/create P: rc = %d\n", rc);
                GOTO(out_O, rc);
        }
        CDEBUG(D_INODE, "putting P: %p, count = %d\n", dentry,
               atomic_read(&dentry->d_count) - 1);
        f_dput(dentry);
        dentry = simple_mkdir(current->fs->pwd, "D", 0700);
        CDEBUG(D_INODE, "got/created D: %p\n", dentry);
        if (IS_ERR(dentry)) {
                rc = PTR_ERR(dentry);
                CERROR("cannot open/create D: rc = %d\n", rc);
                GOTO(out_O, rc);
        }
        CDEBUG(D_INODE, "putting D: %p, count = %d\n", dentry,
               atomic_read(&dentry->d_count) - 1);
        f_dput(dentry);

        /*
         * Create directories and/or get dentries for each object type.
         * This saves us from having to do multiple lookups for each one.
         */
        for (mode = 0; mode < (S_IFMT >> S_SHIFT); mode++) {
                char *type = obd_type_by_mode[mode];

                if (!type) {
                        filter->fo_dentry_O_mode[mode] = NULL;
                        continue;
                }
                dentry = simple_mkdir(filter->fo_dentry_O, type, 0700);
                CDEBUG(D_INODE, "got/created O/%s: %p\n", type, dentry);
                if (IS_ERR(dentry)) {
                        rc = PTR_ERR(dentry);
                        CERROR("cannot create O/%s: rc = %d\n", type, rc);
                        GOTO(out_O_mode, rc);
                }
                filter->fo_dentry_O_mode[mode] = dentry;
        }

        filter_id(rootid, FILTER_ROOTINO, S_IFDIR);
        file = filp_open(rootid, O_RDWR | O_CREAT, 0755);
        if (IS_ERR(file)) {
                rc = PTR_ERR(file);
                CERROR("OBD filter: cannot open/create root %s: rc = %d\n",
                       rootid, rc);
                GOTO(out_O_mode, rc);
        }
        filp_close(file, 0);

        file = filp_open("D/status", O_RDWR | O_CREAT, 0700);
        if ( !file || IS_ERR(file) ) {
                rc = PTR_ERR(file);
                CERROR("OBD filter: cannot open/create status %s: rc = %d\n",
                       "D/status", rc);
                GOTO(out_O_mode, rc);
        }

        /* steal operations */
        inode = file->f_dentry->d_inode;
        filter->fo_fop = file->f_op;
        filter->fo_iop = inode->i_op;
        filter->fo_aops = inode->i_mapping->a_ops;

        if (inode->i_size == 0) {
                __u64 disk_lastino = cpu_to_le64(lastino);
                ssize_t retval = file->f_op->write(file, (char *)&disk_lastino,
                                                   sizeof(disk_lastino),
                                                   &file->f_pos);
                if (retval != sizeof(disk_lastino)) {
                        CDEBUG(D_INODE, "OBD filter: error writing lastino\n");
                        filp_close(file, 0);
                        GOTO(out_O_mode, rc = -EIO);
                }
        } else {
                __u64 disk_lastino;
                ssize_t retval = file->f_op->read(file, (char *)&disk_lastino,
                                                  sizeof(disk_lastino),
                                                  &file->f_pos);
                if (retval != sizeof(disk_lastino)) {
                        CDEBUG(D_INODE, "OBD filter: error reading lastino\n");
                        filp_close(file, 0);
                        GOTO(out_O_mode, rc = -EIO);
                }
                lastino = le64_to_cpu(disk_lastino);
        }
        filter->fo_lastino = lastino;
        filp_close(file, 0);

        rc = 0;
 out:
        pop_ctxt(&saved);

        return(rc);

out_O_mode:
        while (mode-- > 0) {
                struct dentry *dentry = filter->fo_dentry_O_mode[mode];
                if (dentry) {
                        f_dput(dentry);
                        filter->fo_dentry_O_mode[mode] = NULL;
                }
        }
out_O:
        f_dput(filter->fo_dentry_O);
        filter->fo_dentry_O = NULL;
        goto out;
}

/* cleanup the filter: write last used object id to status file */
static void filter_post(struct obd_device *obddev)
{
        struct obd_run_ctxt saved;
        struct filter_obd *filter = &obddev->u.filter;
        __u64 disk_lastino;
        long rc;
        struct file *file;
        int mode;

        push_ctxt(&saved, &filter->fo_ctxt);
        file = filp_open("D/status", O_RDWR | O_CREAT, 0700);
        if (IS_ERR(file)) {
                CERROR("OBD filter: cannot create status file\n");
                goto out;
        }

        file->f_pos = 0;
        disk_lastino = cpu_to_le64(filter->fo_lastino);
        rc = file->f_op->write(file, (char *)&disk_lastino,
                       sizeof(disk_lastino), &file->f_pos);
        if (rc != sizeof(disk_lastino))
                CERROR("OBD filter: error writing lastino: rc = %ld\n", rc);

        rc = filp_close(file, NULL);
        if (rc)
                CERROR("OBD filter: cannot close status file: rc = %ld\n", rc);

        for (mode = 0; mode < (S_IFMT >> S_SHIFT); mode++) {
                struct dentry *dentry = filter->fo_dentry_O_mode[mode];
                if (dentry) {
                        f_dput(dentry);
                        filter->fo_dentry_O_mode[mode] = NULL;
                }
        }
        f_dput(filter->fo_dentry_O);
out:
        pop_ctxt(&saved);
}


static __u64 filter_next_id(struct obd_device *obddev)
{
        obd_id id;

        spin_lock(&obddev->u.filter.fo_lock);
        id = ++obddev->u.filter.fo_lastino;
        spin_unlock(&obddev->u.filter.fo_lock);

        /* FIXME: write the lastino to disk here */
        return id;
}

/* how to get files, dentries, inodes from object id's */
/* parent i_sem is already held if needed for exclusivity */
static struct dentry *filter_fid2dentry(struct obd_device *obddev,
                                        struct dentry *dparent,
                                        __u64 id, __u32 type)
{
        struct super_block *sb = obddev->u.filter.fo_sb;
        struct dentry *dchild;
        char name[32];
        int len;
        ENTRY;

        if (!sb || !sb->s_dev) {
                CERROR("fatal: device not initialized.\n");
                RETURN(ERR_PTR(-ENXIO));
        }

        if (id == 0) {
                CERROR("fatal: invalid object #0\n");
                LBUG();
                RETURN(ERR_PTR(-ESTALE));
        }

        if (!(type & S_IFMT)) {
                CERROR("OBD %s, object %Lu has bad type: %o\n", __FUNCTION__,
                       (unsigned long long)id, type);
                RETURN(ERR_PTR(-EINVAL));
        }

        len = sprintf(name, "%Ld", id);
        CDEBUG(D_INODE, "opening object O/%s/%s\n", obd_mode_to_type(type),
               name);
        dchild = lookup_one_len(name, dparent, len);
        if (IS_ERR(dchild)) {
                CERROR("child lookup error %ld\n", PTR_ERR(dchild));
                RETURN(dchild);
        }

        CDEBUG(D_INODE, "got child obj O/%s/%s: %p, count = %d\n",
               obd_mode_to_type(type), name, dchild,
               atomic_read(&dchild->d_count));

        LASSERT(atomic_read(&dchild->d_count) > 0);

        RETURN(dchild);
}

static struct file *filter_obj_open(struct obd_device *obddev,
                                    __u64 id, __u32 type)
{
        struct super_block *sb = obddev->u.filter.fo_sb;
        struct obd_run_ctxt saved;
        char name[24];
        struct file *file;
        ENTRY;

        if (!sb || !sb->s_dev) {
                CERROR("fatal: device not initialized.\n");
                RETURN(ERR_PTR(-ENXIO));
        }

        if (!id) {
                CERROR("fatal: invalid obdo %Lu\n", (unsigned long long)id);
                RETURN(ERR_PTR(-ESTALE));
        }

        if (!(type & S_IFMT)) {
                CERROR("OBD %s, no type (%Ld), mode %o!\n", __FUNCTION__,
                       (unsigned long long)id, type);
                RETURN(ERR_PTR(-EINVAL));
        }

        filter_id(name, id, type);
        push_ctxt(&saved, &obddev->u.filter.fo_ctxt);
        file = filp_open(name, O_RDONLY | O_LARGEFILE, 0 /* type? */);
        pop_ctxt(&saved);

        CDEBUG(D_INODE, "opening obdo %s: rc = %p\n", name, file);

        if (IS_ERR(file))
                file = NULL;
        RETURN(file);
}

static struct dentry *filter_parent(struct obd_device *obddev, obd_mode mode)
{
        struct filter_obd *filter = &obddev->u.filter;

        return filter->fo_dentry_O_mode[(mode & S_IFMT) >> S_SHIFT];
}


/* obd methods */
static int filter_connect(struct lustre_handle *conn, struct obd_device *obd,
                          char *cluuid)
{
        int rc;
        ENTRY;
        MOD_INC_USE_COUNT;
        rc = class_connect(conn, obd, cluuid);
        if (rc)
                MOD_DEC_USE_COUNT;
        RETURN(rc);
}

static int filter_disconnect(struct lustre_handle *conn)
{
        int rc;
        ENTRY;

        rc = class_disconnect(conn);
        if (!rc)
                MOD_DEC_USE_COUNT;

        /* XXX cleanup preallocated inodes */
        RETURN(rc);
}

/* mount the file system (secretly) */
static int filter_setup(struct obd_device *obddev, obd_count len, void *buf)
{
        struct obd_ioctl_data* data = buf;
        struct filter_obd *filter;
        struct vfsmount *mnt;
        int err = 0;
        ENTRY;

        if (!data->ioc_inlbuf1 || !data->ioc_inlbuf2)
                RETURN(-EINVAL);

        MOD_INC_USE_COUNT;
        mnt = do_kern_mount(data->ioc_inlbuf2, 0, data->ioc_inlbuf1, NULL);
        err = PTR_ERR(mnt);
        if (IS_ERR(mnt))
                GOTO(err_dec, err);

        filter = &obddev->u.filter;;
        filter->fo_vfsmnt = mnt;
        filter->fo_fstype = strdup(data->ioc_inlbuf2);
        filter->fo_sb = mnt->mnt_root->d_inode->i_sb;
        /* XXX is this even possible if do_kern_mount succeeded? */
        if (!filter->fo_sb)
                GOTO(err_kfree, err = -ENODEV);

        OBD_SET_CTXT_MAGIC(&filter->fo_ctxt);
        filter->fo_ctxt.pwdmnt = mnt;
        filter->fo_ctxt.pwd = mnt->mnt_root;
        filter->fo_ctxt.fs = get_ds();

        err = filter_prep(obddev);
        if (err)
                GOTO(err_kfree, err);
        spin_lock_init(&filter->fo_lock);

        obddev->obd_namespace =
                ldlm_namespace_new("filter-tgt", LDLM_NAMESPACE_SERVER);
        if (obddev->obd_namespace == NULL)
                LBUG();

        ptlrpc_init_client(LDLM_REQUEST_PORTAL, LDLM_REPLY_PORTAL,
                           "filter_ldlm_client", &obddev->obd_ldlm_client);

        RETURN(0);

err_kfree:
        kfree(filter->fo_fstype);
        unlock_kernel();
        mntput(filter->fo_vfsmnt);
        filter->fo_sb = 0;
        lock_kernel();

err_dec:
        MOD_DEC_USE_COUNT;
        return err;
}


static int filter_cleanup(struct obd_device * obddev)
{
        struct super_block *sb;
        ENTRY;

        if (!list_empty(&obddev->obd_exports)) {
                CERROR("still has clients!\n");
                class_disconnect_all(obddev);
                if (!list_empty(&obddev->obd_exports)) {
                        CERROR("still has exports after forced cleanup?\n");
                        RETURN(-EBUSY);
                }
        }

        ldlm_namespace_free(obddev->obd_namespace);

        sb = obddev->u.filter.fo_sb;
        if (!obddev->u.filter.fo_sb)
                RETURN(0);

        filter_post(obddev);

        shrink_dcache_parent(sb->s_root);
        unlock_kernel();
        mntput(obddev->u.filter.fo_vfsmnt);
        obddev->u.filter.fo_sb = 0;
        kfree(obddev->u.filter.fo_fstype);

        lock_kernel();

        MOD_DEC_USE_COUNT;
        RETURN(0);
}


static inline void filter_from_inode(struct obdo *oa, struct inode *inode,
                                     int valid)
{
        int type = oa->o_mode & S_IFMT;
        ENTRY;

        CDEBUG(D_INFO, "src inode %ld (%p), dst obdo %ld valid 0x%08x\n",
               inode->i_ino, inode, (long)oa->o_id, valid);
        /* Don't copy the inode number in place of the object ID */
        obdo_from_inode(oa, inode, valid);
        oa->o_mode &= ~S_IFMT;
        oa->o_mode |= type;

        if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode)) {
                obd_rdev rdev = kdev_t_to_nr(inode->i_rdev);
                oa->o_rdev = rdev;
                oa->o_valid |= OBD_MD_FLRDEV;
        }

        EXIT;
}

static int filter_getattr(struct lustre_handle *conn, struct obdo *oa,
                          struct lov_stripe_md *md)
{
        struct obd_device *obddev = class_conn2obd(conn);
        struct dentry *dentry;
        ENTRY;

        if (!class_conn2export(conn)) {
                CDEBUG(D_IOCTL, "fatal: invalid client "LPX64"\n", conn->addr);
                RETURN(-EINVAL);
        }

        obddev = class_conn2obd(conn);
        dentry = filter_fid2dentry(obddev, filter_parent(obddev, oa->o_mode),
                                   oa->o_id, oa->o_mode);
        if (IS_ERR(dentry))
                RETURN(PTR_ERR(dentry));

        filter_from_inode(oa, dentry->d_inode, oa->o_valid);

        f_dput(dentry);
        RETURN(0);
}

static int filter_setattr(struct lustre_handle *conn, struct obdo *oa,
                          struct lov_stripe_md *md)
{
        struct obd_run_ctxt saved;
        struct obd_device *obd = class_conn2obd(conn);
        struct dentry *dentry;
        struct iattr iattr;
        struct inode *inode;
        int rc;
        ENTRY;

        iattr_from_obdo(&iattr, oa, oa->o_valid);
        iattr.ia_mode = (iattr.ia_mode & ~S_IFMT) | S_IFREG;
        dentry = filter_fid2dentry(obd, filter_parent(obd, iattr.ia_mode),
                                   oa->o_id, iattr.ia_mode);
        if (IS_ERR(dentry))
                RETURN(PTR_ERR(dentry));

        inode = dentry->d_inode;
        if (!inode) {
                CERROR("setattr on non-existent object: "LPU64"\n", oa->o_id);
                GOTO(out_setattr, rc = -ENOENT);
        }

        lock_kernel();
        if (iattr.ia_valid & ATTR_SIZE)
                down(&inode->i_sem);
        push_ctxt(&saved, &obd->u.filter.fo_ctxt);
        if (inode->i_op->setattr)
                rc = inode->i_op->setattr(dentry, &iattr);
        else
                rc = inode_setattr(inode, &iattr);
        pop_ctxt(&saved);
        if (iattr.ia_valid & ATTR_SIZE) {
                up(&inode->i_sem);
                oa->o_valid = OBD_MD_FLBLOCKS | OBD_MD_FLCTIME | OBD_MD_FLMTIME;
                obdo_from_inode(oa, inode, oa->o_valid);
        }
        unlock_kernel();

out_setattr:
        f_dput(dentry);
        RETURN(rc);
}

static int filter_open(struct lustre_handle *conn, struct obdo *oa,
                          struct lov_stripe_md *ea)
{
        struct obd_export *export;
        struct obd_device *obd;
        struct dentry *dentry;
        /* ENTRY; */

        export = class_conn2export(conn);
        if (!export) {
                CDEBUG(D_IOCTL, "fatal: invalid client "LPX64"\n", conn->addr);
                RETURN(-EINVAL);
        }

        obd = export->exp_obd;
        dentry = filter_fid2dentry(obd, filter_parent(obd, oa->o_mode),
                                   oa->o_id, oa->o_mode);
        if (IS_ERR(dentry))
                RETURN(PTR_ERR(dentry));

        filter_from_inode(oa, dentry->d_inode, OBD_MD_FLSIZE | OBD_MD_FLBLOCKS |
                          OBD_MD_FLMTIME | OBD_MD_FLCTIME);

        return 0;
} /* filter_open */

static int filter_close(struct lustre_handle *conn, struct obdo *oa,
                        struct lov_stripe_md *ea)
{
        struct obd_device *obd;
        struct dentry *dentry;
        /* ENTRY; */

        obd = class_conn2obd(conn);
        if (!obd) {
                CDEBUG(D_IOCTL, "fatal: invalid client "LPX64"\n", conn->addr);
                RETURN(-EINVAL);
        }

        dentry = filter_fid2dentry(obd, filter_parent(obd, oa->o_mode),
                                   oa->o_id, oa->o_mode);
        if (IS_ERR(dentry))
                RETURN(PTR_ERR(dentry));
        LASSERT(atomic_read(&dentry->d_count) > 1);

        f_dput(dentry);  /* for the close */
        f_dput(dentry);  /* for this call */
        return 0;
} /* filter_close */

static int filter_create(struct lustre_handle* conn, struct obdo *oa,
                         struct lov_stripe_md **ea)
{
        char name[64];
        struct obd_run_ctxt saved;
        struct file *file;
        int mode;
        struct obd_device *obd = class_conn2obd(conn);
        struct iattr;
        ENTRY;

        if (!obd) {
                CERROR("invalid client "LPX64"\n", conn->addr);
                return -EINVAL;
        }

        if (!(oa->o_mode && S_IFMT)) {
                CERROR("filter obd: no type!\n");
                return -ENOENT;
        }

        oa->o_id = filter_next_id(obd);

        filter_id(name, oa->o_id, oa->o_mode);
        mode = (oa->o_mode & ~S_IFMT) | S_IFREG;
        push_ctxt(&saved, &obd->u.filter.fo_ctxt);
        file = filp_open(name, O_RDONLY | O_CREAT, mode);
        pop_ctxt(&saved);
        if (IS_ERR(file)) {
                CERROR("Error mknod obj %s, err %ld\n", name, PTR_ERR(file));
                return -ENOENT;
        }

        /* Set flags for fields we have set in the inode struct */
        oa->o_valid = OBD_MD_FLID | OBD_MD_FLBLKSZ | OBD_MD_FLBLOCKS |
                 OBD_MD_FLMTIME | OBD_MD_FLATIME | OBD_MD_FLCTIME;
        filter_from_inode(oa, file->f_dentry->d_inode, oa->o_valid);
        filp_close(file, 0);

        return 0;
}

static int filter_destroy(struct lustre_handle *conn, struct obdo *oa, 
                          struct lov_stripe_md *ea)
{
        struct obd_device *obd;
        struct filter_obd *filter;
        struct obd_run_ctxt saved;
        struct inode *inode;
        struct dentry *dir_dentry, *object_dentry;
        int rc;
        ENTRY;

        obd = class_conn2obd(conn);
        if (!obd) {
                CERROR("invalid client "LPX64"\n", conn->addr);
                RETURN(-EINVAL);
        }

        CDEBUG(D_INODE, "destroying object "LPD64"\n", oa->o_id);

        dir_dentry = filter_parent(obd, oa->o_mode);
        down(&dir_dentry->d_inode->i_sem);

        object_dentry = filter_fid2dentry(obd, dir_dentry, oa->o_id,
                                          oa->o_mode);
        if (IS_ERR(object_dentry))
                GOTO(out, rc = -ENOENT);

        inode = object_dentry->d_inode;
        if (inode == NULL) {
                CERROR("trying to destroy negative inode "LPX64"!\n", oa->o_id);
                GOTO(out, rc = -ENOENT);
        }

        if (inode->i_nlink != 1) {
                CERROR("destroying inode with nlink = %d\n", inode->i_nlink);
                LBUG();
                inode->i_nlink = 1;
        }
        inode->i_mode = S_IFREG;

        filter = &obd->u.filter;
        push_ctxt(&saved, &filter->fo_ctxt);

        rc = vfs_unlink(dir_dentry->d_inode, object_dentry);
        pop_ctxt(&saved);
        f_dput(object_dentry);

        EXIT;
out:
        up(&dir_dentry->d_inode->i_sem);
        return rc;
}

/* NB count and offset are used for punch, but not truncate */
static int filter_truncate(struct lustre_handle *conn, struct obdo *oa,
                           struct lov_stripe_md *md,
                           obd_off start, obd_off end)
{
        int error;
        ENTRY;

        if (end != OBD_PUNCH_EOF)
                CERROR("PUNCH not supported, only truncate works\n");

        CDEBUG(D_INODE, "calling truncate for object "LPX64", valid = %x, "
               "o_size = "LPD64"\n", oa->o_id, oa->o_valid, start);
        oa->o_size = start;
        error = filter_setattr(conn, oa, NULL);
        RETURN(error);
}

static int filter_pgcache_brw(int cmd, struct lustre_handle *conn,
                              struct lov_stripe_md *md, obd_count oa_bufs,
                              struct brw_page *pga, brw_callback_t callback,
                              struct io_cb_data *data)
{
        struct obd_run_ctxt      saved;
        struct super_block      *sb;
        int                      pnum;          /* index to pages (bufs) */
        unsigned long            retval;
        int                      error;
        struct file             *file;
        struct obd_device       *obd = class_conn2obd(conn);
        int pg;
        ENTRY;

        if (!obd) {
                CDEBUG(D_IOCTL, "invalid client "LPX64"\n", conn->addr);
                RETURN(-EINVAL);
        }

        sb = obd->u.filter.fo_sb;
        push_ctxt(&saved, &obd->u.filter.fo_ctxt);
        pnum = 0; /* pnum indexes buf 0..num_pages */

        file = filter_obj_open(obd, md->lmd_object_id, S_IFREG);
        if (IS_ERR(file))
                GOTO(out, retval = PTR_ERR(file));

        /* count doubles as retval */
        for (pg = 0; pg < oa_bufs; pg++) {
                CDEBUG(D_INODE, "OP %d obdo pgno: (%d) (%ld,"LPU64
                       ") off count ("LPU64",%d)\n",
                       cmd, pnum, file->f_dentry->d_inode->i_ino,
                       pga[pnum].off >> PAGE_CACHE_SHIFT, pga[pnum].off,
                       (int)pga[pnum].count);
                if (cmd & OBD_BRW_WRITE) {
                        loff_t off;
                        char *buffer;
                        off = pga[pnum].off;
                        buffer = kmap(pga[pnum].pg);
                        retval = file->f_op->write(file, buffer,
                                                   pga[pnum].count,
                                                   &off);
                        kunmap(pga[pnum].pg);
                        CDEBUG(D_INODE, "retval %ld\n", retval);
                } else {
                        loff_t off = pga[pnum].off;
                        char *buffer = kmap(pga[pnum].pg);

                        if (off >= file->f_dentry->d_inode->i_size) {
                                memset(buffer, 0, pga[pnum].count);
                                retval = pga[pnum].count;
                        } else {
                                retval = file->f_op->read(file, buffer,
                                                          pga[pnum].count, &off);
                        }
                        kunmap(pga[pnum].pg);

                        if (retval != pga[pnum].count) {
                                filp_close(file, 0);
                                GOTO(out, retval = -EIO);
                        }
                        CDEBUG(D_INODE, "retval %ld\n", retval);
                }
                pnum++;
        }
        /* sizes and blocks are set by generic_file_write */
        /* ctimes/mtimes will follow with a setattr call */
        filp_close(file, 0);

        /* XXX: do something with callback if it is set? */

        EXIT;
out:
        pop_ctxt(&saved);
        error = (retval >= 0) ? 0 : retval;
        return error;
}

/*
 * Calculate the number of buffer credits needed to write multiple pages in
 * a single ext3/extN transaction.  No, this shouldn't be here, but as yet
 * ext3 doesn't have a nice API for calculating this sort of thing in advance.
 *
 * See comment above ext3_writepage_trans_blocks for details.  We assume
 * no data journaling is being done, but it does allow for all of the pages
 * being non-contiguous.  If we are guaranteed contiguous pages we could
 * reduce the number of (d)indirect blocks a lot.
 *
 * With N blocks per page and P pages, for each inode we have at most:
 * N*P indirect
 * min(N*P, blocksize/4 + 1) dindirect blocks
 * 1 tindirect
 *
 * For the entire filesystem, we have at most:
 * min(sum(nindir + P), ngroups) bitmap blocks (from the above)
 * min(sum(nindir + P), gdblocks) group descriptor blocks (from the above)
 * 1 inode block
 * 1 superblock
 * 2 * EXT3_SINGLEDATA_TRANS_BLOCKS for the quota files
 */
static int ext3_credits_needed(struct super_block *sb, int objcount,
                               struct obd_ioobj *obj)
{
        struct obd_ioobj *o = obj;
        int blockpp = 1 << (PAGE_CACHE_SHIFT - sb->s_blocksize_bits);
        int addrpp = EXT3_ADDR_PER_BLOCK(sb) * blockpp;
        int nbitmaps = 0;
        int ngdblocks = 0;
        int needed = objcount + 1;
        int i;

        for (i = 0; i < objcount; i++, o++) {
                int nblocks = o->ioo_bufcnt * blockpp;
                int ndindirect = min(nblocks, addrpp + 1);
                int nindir = nblocks + ndindirect + 1;

                nbitmaps += nindir + nblocks;
                ngdblocks += nindir + nblocks;

                needed += nindir;
        }

        if (nbitmaps > EXT3_SB(sb)->s_groups_count)
                nbitmaps = EXT3_SB(sb)->s_groups_count;
        if (ngdblocks > EXT3_SB(sb)->s_gdb_count)
                ngdblocks = EXT3_SB(sb)->s_gdb_count;

        needed += nbitmaps + ngdblocks;

#ifdef CONFIG_QUOTA
        /* We assume that there will be 1 bit set in s_dquot.flags for each
         * quota file that is active.  This is at least true for now.
         */
        needed += hweight32(sb_any_quota_enabled(sb)) *
                EXT3_SINGLEDATA_TRANS_BLOCKS;
#endif

        return needed;
}

/* We have to start a huge journal transaction here to hold all of the
 * metadata for the pages being written here.  This is necessitated by
 * the fact that we do lots of prepare_write operations before we do
 * any of the matching commit_write operations, so even if we split
 * up to use "smaller" transactions none of them could complete until
 * all of them were opened.  By having a single journal transaction,
 * we eliminate duplicate reservations for common blocks like the
 * superblock and group descriptors or bitmaps.
 *
 * We will start the transaction here, but each prepare_write will
 * add a refcount to the transaction, and each commit_write will
 * remove a refcount.  The transaction will be closed when all of
 * the pages have been written.
 */
static void *ext3_filter_journal_start(struct filter_obd *filter,
                                       int objcount, struct obd_ioobj *obj,
                                       int niocount, struct niobuf_remote *nb)
{
        journal_t *journal = NULL;
        handle_t *handle = NULL;
        int needed;

        /* Assumes ext3 and extN have same sb_info layout, but avoids issues
         * with having extN built properly before filterobd for now.
         */
        journal = EXT3_SB(filter->fo_sb)->s_journal;
        needed = ext3_credits_needed(filter->fo_sb, objcount, obj);

        /* The number of blocks we could _possibly_ dirty can very large.
         * We reduce our request if it is absurd (and we couldn't get that
         * many credits for a single handle anyways).
         *
         * At some point we have to limit the size of I/Os sent at one time,
         * increase the size of the journal, or we have to calculate the
         * actual journal requirements more carefully by checking all of
         * the blocks instead of being maximally pessimistic.  It remains to
         * be seen if this is a real problem or not.
         */
        if (needed > journal->j_max_transaction_buffers) {
                CERROR("want too many journal credits (%d) using %d instead\n",
                       needed, journal->j_max_transaction_buffers);
                needed = journal->j_max_transaction_buffers;
        }

        handle = journal_start(journal, needed);
        if (IS_ERR(handle))
                CERROR("can't get handle for %d credits: rc = %ld\n", needed,
                       PTR_ERR(handle));

        return(handle);
}

static void *filter_journal_start(void **journal_save,
                                  struct filter_obd *filter,
                                  int objcount, struct obd_ioobj *obj,
                                  int niocount, struct niobuf_remote *nb)
{
        void *handle = NULL;

        /* This may not be necessary - we probably never have a
         * transaction started when we enter here, so we can
         * remove the saving of the journal state entirely.
         * For now leave it in just to see if it ever happens.
         */
        *journal_save = current->journal_info;
        if (*journal_save) {
                CERROR("Already have handle %p???\n", *journal_save);
                LBUG();
                current->journal_info = NULL;
        }

        if (!strcmp(filter->fo_fstype, "ext3") ||
            !strcmp(filter->fo_fstype, "extN"))
                handle = ext3_filter_journal_start(filter, objcount, obj,
                                                   niocount, nb);
        return handle;
}

static int ext3_filter_journal_stop(void *handle)
{
        int rc;

        /* We got a refcount on the handle for each call to prepare_write,
         * so we can drop the "parent" handle here to avoid the need for
         * osc to call back into filterobd to close the handle.  The
         * remaining references will be dropped in commit_write.
         */
        rc = journal_stop((handle_t *)handle);

        return rc;
}

static int filter_journal_stop(void *journal_save, struct filter_obd *filter,
                               void *handle)
{
        int rc = 0;

        if (!strcmp(filter->fo_fstype, "ext3") ||
            !strcmp(filter->fo_fstype, "extN"))
                rc = ext3_filter_journal_stop(handle);

        if (rc)
                CERROR("error on journal stop: rc = %d\n", rc);

        current->journal_info = journal_save;

        return rc;
}

static inline void lustre_put_page(struct page *page)
{
        kunmap(page);
        page_cache_release(page);
}

static struct page *
lustre_get_page_read(struct inode *inode, unsigned long index)
{
        struct address_space *mapping = inode->i_mapping;
        struct page *page;
        int rc;

        page = read_cache_page(mapping, index,
                               (filler_t*)mapping->a_ops->readpage, NULL);
        if (!IS_ERR(page)) {
                wait_on_page(page);
                kmap(page);
                if (!Page_Uptodate(page)) {
                        CERROR("page index %lu not uptodate\n", index);
                        GOTO(err_page, rc = -EIO);
                }
                if (PageError(page)) {
                        CERROR("page index %lu has error\n", index);
                        GOTO(err_page, rc = -EIO);
                }
        }
        return page;

err_page:
        lustre_put_page(page);
        return ERR_PTR(rc);
}

static struct page *
lustre_get_page_write(struct inode *inode, unsigned long index)
{
        struct address_space *mapping = inode->i_mapping;
        struct page *page;
        int rc;

        page = grab_cache_page(mapping, index); /* locked page */

        if (!IS_ERR(page)) {
                kmap(page);
                /* Note: Called with "O" and "PAGE_SIZE" this is essentially
                 * a no-op for most filesystems, because we write the whole
                 * page.  For partial-page I/O this will read in the page.
                 */
                rc = mapping->a_ops->prepare_write(NULL, page, 0, PAGE_SIZE);
                if (rc) {
                        CERROR("page index %lu, rc = %d\n", index, rc);
                        if (rc != -ENOSPC)
                                LBUG();
                        GOTO(err_unlock, rc);
                }
                /* XXX not sure if we need this if we are overwriting page */
                if (PageError(page)) {
                        CERROR("error on page index %lu, rc = %d\n", index, rc);
                        LBUG();
                        GOTO(err_unlock, rc = -EIO);
                }
        }
        return page;

err_unlock:
        unlock_page(page);
        lustre_put_page(page);
        return ERR_PTR(rc);
}

static int lustre_commit_write(struct page *page, unsigned from, unsigned to)
{
        struct inode *inode = page->mapping->host;
        int err;

        err = page->mapping->a_ops->commit_write(NULL, page, from, to);
        if (!err && IS_SYNC(inode))
                err = waitfor_one_page(page);

        //SetPageUptodate(page); // the client commit_write will do this

        SetPageReferenced(page);
        unlock_page(page);
        lustre_put_page(page);
        return err;
}

struct page *filter_get_page_write(struct inode *inode, unsigned long index,
                                   struct niobuf_local *lnb, int *pglocked)
{
        struct address_space *mapping = inode->i_mapping;
        struct page *page;
        int rc;

        //ASSERT_PAGE_INDEX(index, GOTO(err, rc = -EINVAL));
        if (*pglocked)
                page = grab_cache_page_nowait(mapping, index); /* locked page */
        else
                page = grab_cache_page(mapping, index); /* locked page */


        /* This page is currently locked, so get a temporary page instead. */
        /* XXX I believe this is a very dangerous thing to do - consider if
         *     we had multiple writers for the same file (definitely the case
         *     if we are using this codepath).  If writer A locks the page,
         *     writer B writes to a copy (as here), writer A drops the page
         *     lock, and writer C grabs the lock before B does, then B will
         *     later overwrite the data from C, even if C had LDLM locked
         *     and initiated the write after B did.
         */
        if (!page) {
                unsigned long addr;
                CDEBUG(D_PAGE, "ino %ld page %ld locked\n", inode->i_ino,index);
                addr = __get_free_pages(GFP_KERNEL, 0); /* locked page */
                if (!addr) {
                        CERROR("no memory for a temp page\n");
                        LBUG();
                        GOTO(err, rc = -ENOMEM);
                }
                page = virt_to_page(addr);
                kmap(page);
                page->index = index;
                lnb->flags |= N_LOCAL_TEMP_PAGE;
        } else if (!IS_ERR(page)) {
                (*pglocked)++;
                kmap(page);

                /* Note: Called with "O" and "PAGE_SIZE" this is essentially
                 * a no-op for most filesystems, because we write the whole
                 * page.  For partial-page I/O this will read in the page.
                 */
                rc = mapping->a_ops->prepare_write(NULL, page, 0, PAGE_SIZE);
                if (rc) {
                        CERROR("page index %lu, rc = %d\n", index, rc);
                        if (rc != -ENOSPC)
                                LBUG();
                        GOTO(err_unlock, rc);
                }
                /* XXX not sure if we need this if we are overwriting page */
                if (PageError(page)) {
                        CERROR("error on page index %lu, rc = %d\n", index, rc);
                        LBUG();
                        GOTO(err_unlock, rc = -EIO);
                }
        }
        return page;

err_unlock:
        unlock_page(page);
        lustre_put_page(page);
err:
        return ERR_PTR(rc);
}

/*
 * We need to balance prepare_write() calls with commit_write() calls.
 * If the page has been prepared, but we have no data for it, we don't
 * want to overwrite valid data on disk, but we still need to zero out
 * data for space which was newly allocated.  Like part of what happens
 * in __block_prepare_write() for newly allocated blocks.
 *
 * XXX currently __block_prepare_write() creates buffers for all the
 *     pages, and the filesystems mark these buffers as BH_New if they
 *     were newly allocated from disk. We use the BH_New flag similarly.
 */
static int filter_commit_write(struct page *page, unsigned from, unsigned to,
                               int err)
{
        if (err) {
                unsigned block_start, block_end;
                struct buffer_head *bh, *head = page->buffers;
                unsigned blocksize = head->b_size;
                void *addr = page_address(page);

                /* debugging: just seeing if this ever happens */
                CERROR("called filter_commit_write for obj %ld:%ld on err %d\n",
                       page->index, page->mapping->host->i_ino, err);

                /* Currently one buffer per page, but in the future... */
                for (bh = head, block_start = 0; bh != head || !block_start;
                     block_start = block_end, bh = bh->b_this_page) {
                        block_end = block_start + blocksize;
                        if (buffer_new(bh))
                                memset(addr + block_start, 0, blocksize);
                }
        }

        return lustre_commit_write(page, from, to);
}

static int filter_preprw(int cmd, struct lustre_handle *conn,
                         int objcount, struct obd_ioobj *obj,
                         int niocount, struct niobuf_remote *nb,
                         struct niobuf_local *res, void **desc_private)
{
        struct obd_run_ctxt saved;
        struct obd_device *obd;
        struct obd_ioobj *o = obj;
        struct niobuf_remote *b = nb;
        struct niobuf_local *r = res;
        void *journal_save = NULL;
        int pglocked = 0;
        int rc = 0;
        int i;
        ENTRY;

        obd = class_conn2obd(conn);
        if (!obd) {
                CDEBUG(D_IOCTL, "invalid client "LPX64"\n", conn->addr);
                RETURN(-EINVAL);
        }
        memset(res, 0, sizeof(*res) * niocount);

        push_ctxt(&saved, &obd->u.filter.fo_ctxt);

        if (cmd & OBD_BRW_WRITE) {
                *desc_private = filter_journal_start(&journal_save,
                                                     &obd->u.filter,
                                                     objcount, obj, niocount,
                                                     nb);
                if (IS_ERR(*desc_private))
                        GOTO(out_ctxt, rc = PTR_ERR(*desc_private));
        }

        for (i = 0; i < objcount; i++, o++) {
                struct dentry *dentry;
                struct inode *inode;
                int j;

                dentry = filter_fid2dentry(obd, filter_parent(obd, S_IFREG),
                                           o->ioo_id, S_IFREG);
                if (IS_ERR(dentry))
                        GOTO(out_clean, rc = PTR_ERR(dentry));
                inode = dentry->d_inode;
                if (!inode) {
                        CERROR("trying to BRW to non-existent file %Ld\n",
                               (unsigned long long)o->ioo_id);
                        f_dput(dentry);
                        GOTO(out_clean, rc = -ENOENT);
                }

                for (j = 0; j < o->ioo_bufcnt; j++, b++, r++) {
                        unsigned long index = b->offset >> PAGE_SHIFT;
                        struct page *page;

                        if (j == 0)
                                r->dentry = dentry;
                        else
                                r->dentry = dget(dentry);

                        if (cmd & OBD_BRW_WRITE)
                                page = filter_get_page_write(inode, index, r,
                                                             &pglocked);
                        else
                                page = lustre_get_page_read(inode, index);

                        if (IS_ERR(page)) {
                                f_dput(dentry);
                                GOTO(out_clean, rc = PTR_ERR(page));
                        }

                        r->addr = page_address(page);
                        r->offset = b->offset;
                        r->page = page;
                        r->len = b->len;
                }
        }

out_stop:
        if (cmd & OBD_BRW_WRITE) {
                int err = filter_journal_stop(journal_save, &obd->u.filter,
                                              *desc_private);
                if (!rc)
                        rc = err;
        }
out_ctxt:
        pop_ctxt(&saved);
        RETURN(rc);
out_clean:
        while (r-- > res) {
                CERROR("error cleanup on brw\n");
                f_dput(r->dentry);
                if (cmd & OBD_BRW_WRITE)
                        filter_commit_write(r->page, 0, PAGE_SIZE, rc);
                else
                        lustre_put_page(r->page);
        }
        goto out_stop;
}

static int filter_write_locked_page(struct niobuf_local *lnb)
{
        struct page *lpage;
        int rc;

        lpage = lustre_get_page_write(lnb->dentry->d_inode, lnb->page->index);
        if (IS_ERR(lpage)) {
                /* It is highly unlikely that we would ever get an error here.
                 * The page we want to get was previously locked, so it had to
                 * have already allocated the space, and we were just writing
                 * over the same data, so there would be no hole in the file.
                 *
                 * XXX: possibility of a race with truncate could exist, need
                 *      to check that.  There are no guarantees w.r.t.
                 *      write order even on a local filesystem, although the
                 *      normal response would be to return the number of bytes
                 *      successfully written and leave the rest to the app.
                 */
                rc = PTR_ERR(lpage);
                CERROR("error getting locked page index %ld: rc = %d\n",
                       lnb->page->index, rc);
                GOTO(out, rc);
        }

        /* lpage is kmapped in lustre_get_page_write() above and kunmapped in
         * lustre_commit_write() below, lnb->page was kmapped previously in
         * filter_get_page_write() and kunmapped in lustre_put_page() below.
         */
        memcpy(page_address(lpage), page_address(lnb->page), PAGE_SIZE);
        rc = lustre_commit_write(lpage, 0, PAGE_SIZE);
        if (rc)
                CERROR("error committing locked page %ld: rc = %d\n",
                       lnb->page->index, rc);
out:
        lustre_put_page(lnb->page);

        return rc;
}

static int filter_commitrw(int cmd, struct lustre_handle *conn,
                           int objcount, struct obd_ioobj *obj,
                           int niocount, struct niobuf_local *res,
                           void *private)
{
        struct obd_run_ctxt saved;
        struct obd_ioobj *o;
        struct niobuf_local *r;
        struct obd_device *obd = class_conn2obd(conn);
        void *journal_save;
        int found_locked = 0;
        int rc = 0;
        int i;
        ENTRY;

        push_ctxt(&saved, &obd->u.filter.fo_ctxt);
        journal_save = current->journal_info;
        LASSERT(!journal_save);

        current->journal_info = private;
        for (i = 0, o = obj, r = res; i < objcount; i++, o++) {
                int j;
                for (j = 0 ; j < o->ioo_bufcnt ; j++, r++) {
                        struct page *page = r->page;

                        if (!page)
                                LBUG();

                        if (r->flags & N_LOCAL_TEMP_PAGE) {
                                found_locked++;
                                continue;
                        }

                        if (cmd & OBD_BRW_WRITE) {
                                int err = filter_commit_write(page, 0,
                                                              r->len, 0);

                                if (!rc)
                                        rc = err;
                        } else
                                lustre_put_page(page);

                        f_dput(r->dentry);
                }
        }
        current->journal_info = journal_save;

        if (!found_locked)
                goto out_ctxt;

        for (i = 0, o = obj, r = res; i < objcount; i++, o++) {
                int j;
                for (j = 0 ; j < o->ioo_bufcnt ; j++, r++) {
                        int err;
                        if (!(r->flags & N_LOCAL_TEMP_PAGE))
                                continue;

                        err = filter_write_locked_page(r);
                        if (!rc)
                                rc = err;
                        f_dput(r->dentry);
                }
        }

out_ctxt:
        pop_ctxt(&saved);
        RETURN(rc);
}

static int filter_statfs(struct lustre_handle *conn, struct obd_statfs *osfs)
{
        struct obd_device *obd = class_conn2obd(conn);
        struct statfs sfs;
        int rc;

        ENTRY;
        rc = vfs_statfs(obd->u.filter.fo_sb, &sfs);
        if (!rc)
                statfs_pack(osfs, &sfs);

        return rc;
}

static int filter_get_info(struct lustre_handle *conn, obd_count keylen,
                           void *key, obd_count *vallen, void **val)
{
        struct obd_device *obd;
        ENTRY;

        obd = class_conn2obd(conn);
        if (!obd) {
                CDEBUG(D_IOCTL, "invalid client "LPX64"\n", conn->addr);
                RETURN(-EINVAL);
        }

        if ( keylen == strlen("blocksize") &&
             memcmp(key, "blocksize", keylen) == 0 ) {
                *vallen = sizeof(long);
                *val = (void *)(long)obd->u.filter.fo_sb->s_blocksize;
                RETURN(0);
        }

        if ( keylen == strlen("blocksize_bits") &&
             memcmp(key, "blocksize_bits", keylen) == 0 ){
                *vallen = sizeof(long);
                *val = (void *)(long)obd->u.filter.fo_sb->s_blocksize_bits;
                RETURN(0);
        }

        if ( keylen == strlen("root_ino") &&
             memcmp(key, "root_ino", keylen) == 0 ){
                *vallen = sizeof(obd_id);
                *val = (void *)(obd_id)FILTER_ROOTINO;
                RETURN(0);
        }

        CDEBUG(D_IOCTL, "invalid key\n");
        RETURN(-EINVAL);
}

int filter_copy_data(struct lustre_handle *dst_conn, struct obdo *dst,
                  struct lustre_handle *src_conn, struct obdo *src,
                  obd_size count, obd_off offset)
{
        struct page *page;
        struct lov_stripe_md srcmd, dstmd; 
        unsigned long index = 0;
        int err = 0;

        memset(&srcmd, 0, sizeof(srcmd));
        memset(&dstmd, 0, sizeof(dstmd));
        srcmd.lmd_object_id = src->o_id;
        dstmd.lmd_object_id = dst->o_id;

        ENTRY;
        CDEBUG(D_INFO, "src: ino %Ld blocks %Ld, size %Ld, dst: ino %Ld\n",
               (unsigned long long)src->o_id, (unsigned long long)src->o_blocks,
               (unsigned long long)src->o_size, (unsigned long long)dst->o_id);
        page = alloc_page(GFP_USER);
        if (page == NULL)
                RETURN(-ENOMEM);

        while (TryLockPage(page))
                ___wait_on_page(page);

        /* XXX with brw vector I/O, we could batch up reads and writes here,
         *     all we need to do is allocate multiple pages to handle the I/Os
         *     and arrays to handle the request parameters.
         */
        while (index < ((src->o_size + PAGE_SIZE - 1) >> PAGE_SHIFT)) {
                struct brw_page pg; 
                struct io_cb_data *cbd = ll_init_cb();

                if (!cbd) { 
                        err = -ENOMEM;
                        EXIT;
                        break;
                }

                pg.pg = page;
                pg.count = PAGE_SIZE;
                pg.off = (page->index) << PAGE_SHIFT;
                pg.flag = 0;

                page->index = index;
                err = obd_brw(OBD_BRW_READ, src_conn, &srcmd, 1, &pg, 
                              ll_sync_io_cb, cbd);

                if ( err ) {
                        EXIT;
                        break;
                }

                cbd = ll_init_cb();
                if (!cbd) { 
                        err = -ENOMEM;
                        EXIT;
                        break;
                }
                pg.flag = OBD_BRW_CREATE;
                CDEBUG(D_INFO, "Read page %ld ...\n", page->index);

                err = obd_brw(OBD_BRW_WRITE, dst_conn, &dstmd, 1, &pg,
                              ll_sync_io_cb, cbd);

                /* XXX should handle dst->o_size, dst->o_blocks here */
                if ( err ) {
                        EXIT;
                        break;
                }

                CDEBUG(D_INFO, "Wrote page %ld ...\n", page->index);

                index++;
        }
        dst->o_size = src->o_size;
        dst->o_blocks = src->o_blocks;
        dst->o_valid |= OBD_MD_FLSIZE | OBD_MD_FLBLOCKS;
        unlock_page(page);
        __free_page(page);

        RETURN(err);
}


static struct obd_ops filter_obd_ops = {
        o_get_info:    filter_get_info,
        o_setup:       filter_setup,
        o_cleanup:     filter_cleanup,
        o_connect:     filter_connect,
        o_disconnect:  filter_disconnect,
        o_statfs:      filter_statfs,
        o_getattr:     filter_getattr,
        o_create:      filter_create,
        o_setattr:     filter_setattr,
        o_destroy:     filter_destroy,
        o_open:        filter_open,
        o_close:       filter_close,
        o_brw:         filter_pgcache_brw,
        o_punch:       filter_truncate,
        o_preprw:      filter_preprw,
        o_commitrw:    filter_commitrw
#if 0
        o_preallocate: filter_preallocate_inodes,
        o_migrate:     filter_migrate,
        o_copy:        filter_copy_data,
        o_iterate:     filter_iterate
#endif
};


static int __init obdfilter_init(void)
{
        printk(KERN_INFO "Filtering OBD driver  v0.001, info@clusterfs.com\n");
        return class_register_type(&filter_obd_ops, OBD_FILTER_DEVICENAME);
}

static void __exit obdfilter_exit(void)
{
        class_unregister_type(OBD_FILTER_DEVICENAME);
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Filtering OBD driver v1.0");
MODULE_LICENSE("GPL");

module_init(obdfilter_init);
module_exit(obdfilter_exit);
