/*-
 * Copyright (c) 2005 David Xu <davidxu@freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

/*
 * sched_group filesystem implementation.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_capsicum.h"
#include "opt_compat.h"

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/limits.h>
#include <sys/buf.h>
#include <sys/capability.h>
#include <sys/dirent.h>
#include <sys/event.h>
#include <sys/eventhandler.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/posix4.h>
#include <sys/poll.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/sysproto.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/syscallsubr.h>
#include <sys/sysent.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <sys/unistd.h>
#include <sys/vnode.h>
#include <machine/atomic.h>

/*
 * Limits and constants
 */
#define	SGFS_NAMELEN		NAME_MAX
#define SGFS_DELEN		(8 + SGFS_NAMELEN)

/* node types */
typedef enum {
	sgfstype_none = 0,
	sgfstype_root,
	sgfstype_dir,
	sgfstype_this,
	sgfstype_parent,
	sgfstype_file,
	sgfstype_symlink,
} sgfs_type_t;

struct sgfs_node;

/*
 * sgfs_info: describes a sgfs instance
 */
struct sgfs_info {
	struct sx		lock;
	struct sgfs_node	*root;
	struct unrhdr		*unrhdr;
};

struct sgfs_vdata {
	LIST_ENTRY(sgfs_vdata)	link;
	struct sgfs_node	*node;
	struct vnode		*vnode;
	struct task		task;
};

/*
 * sgfs_node: describes a node (file or directory) within a sgfs
 */
struct sgfs_node {
	char			name[SGFS_NAMELEN+1];
	struct sgfs_info	*fsinfo;
	struct sgfs_node	*parent;
	LIST_HEAD(,sgfs_node)	children;
	LIST_ENTRY(sgfs_node)	sibling;
	LIST_HEAD(,sgfs_vdata)	vnodes;
	int			refcount;
	sgfs_type_t		type;
	int			deleted;
	uint32_t		fileno;
	void			*data;
	struct timespec		birth;
	struct timespec		ctime;
	struct timespec		atime;
	struct timespec		mtime;
	uid_t			uid;
	gid_t			gid;
	int			mode;
};

struct sgdata {
};

#define	VTON(vp)	(((struct sgfs_vdata *)((vp)->v_data))->node)
#define VTOSG(vp) 	((struct sgdata *)(VTON(vp)->data))
#define	VFSTOSGFS(m)	((struct sgfs_info *)((m)->mnt_data))

static int	unloadable = 0;
static MALLOC_DEFINE(M_SGATA, "sgdata", "sg data");

/* Only one instance per-system */
static struct sgfs_info		sgfs_data;
static uma_zone_t		sgnode_zone;
static uma_zone_t		sgvdata_zone;
static struct vop_vector	sgfs_vnodeops;

/*
 * Directory structure construction and manipulation
 */
static struct sgfs_node	*sgfs_create_dir(struct sgfs_node *parent,
	const char *name, int namelen, struct ucred *cred, int mode);

#if 0
static struct sgfs_node	*sgfs_create_file(struct sgfs_node *parent,
	const char *name, int namelen, struct ucred *cred, int mode);
#endif
static int	sgfs_destroy(struct sgfs_node *);
static void	sgfs_fileno_alloc(struct sgfs_info *, struct sgfs_node *);
static void	sgfs_fileno_free(struct sgfs_info *, struct sgfs_node *);
static int	sgfs_allocv(struct mount *, struct vnode **, struct sgfs_node *);

static void
sgdata_free(void *data)
{
}

/*
 * Initialize fileno bitmap
 */
static void
sgfs_fileno_init(struct sgfs_info *fs)
{
	struct unrhdr *up;

	up = new_unrhdr(1, INT_MAX, NULL);
	fs->unrhdr = up;
}

/*
 * Tear down fileno bitmap
 */
static void
sgfs_fileno_uninit(struct sgfs_info *fs)
{
	struct unrhdr *up;

	up = fs->unrhdr;
	fs->unrhdr = NULL;
	delete_unrhdr(up);
}

/*
 * Allocate a file number
 */
static void
sgfs_fileno_alloc(struct sgfs_info *fs, struct sgfs_node *node)
{
	/* make sure our parent has a file number */
	if (node->parent && !node->parent->fileno)
		sgfs_fileno_alloc(fs, node->parent);

	switch (node->type) {
	case sgfstype_root:
	case sgfstype_dir:
	case sgfstype_file:
	case sgfstype_symlink:
		node->fileno = alloc_unr(fs->unrhdr);
		break;
	case sgfstype_this:
		KASSERT(node->parent != NULL,
		    ("sgfstype_this node has no parent"));
		node->fileno = node->parent->fileno;
		break;
	case sgfstype_parent:
		KASSERT(node->parent != NULL,
		    ("sgfstype_parent node has no parent"));
		if (node->parent == fs->root) {
			node->fileno = node->parent->fileno;
			break;
		}
		KASSERT(node->parent->parent != NULL,
		    ("sgfstype_parent node has no grandparent"));
		node->fileno = node->parent->parent->fileno;
		break;
	default:
		KASSERT(0,
		    ("sgfs_fileno_alloc() called for unknown type node: %d",
			node->type));
		break;
	}
}

/*
 * Release a file number
 */
static void
sgfs_fileno_free(struct sgfs_info *fs, struct sgfs_node *node)
{
	switch (node->type) {
	case sgfstype_root:
	case sgfstype_dir:
	case sgfstype_file:
	case sgfstype_symlink:
		free_unr(fs->unrhdr, node->fileno);
		break;
	case sgfstype_this:
	case sgfstype_parent:
		/* ignore these, as they don't "own" their file number */
		break;
	default:
		KASSERT(0,
		    ("sgfs_fileno_free() called for unknown type node: %d", 
			node->type));
		break;
	}
}

static __inline struct sgfs_node *
sgnode_alloc(void)
{
	return uma_zalloc(sgnode_zone, M_WAITOK | M_ZERO);
}

static __inline void
sgnode_free(struct sgfs_node *node)
{
	uma_zfree(sgnode_zone, node);
}

static __inline void
sgnode_addref(struct sgfs_node *node)
{
	atomic_fetchadd_int(&node->refcount, 1);
}

static __inline void
sgnode_release(struct sgfs_node *node)
{
	struct sgfs_info *fs;
	int old, exp;

	fs = node->fsinfo;
	old = atomic_fetchadd_int(&node->refcount, -1);
	if (node->type == sgfstype_dir ||
	    node->type == sgfstype_root)
		exp = 3; /* include . and .. */
	else
		exp = 1;
	if (old == exp) {
		int locked = sx_xlocked(&fs->lock);
		if (!locked)
			sx_xlock(&fs->lock);
		sgfs_destroy(node);
		if (!locked)
			sx_xunlock(&fs->lock);
	}
}

/*
 * Add a node to a directory
 */
static int
sgfs_add_node(struct sgfs_node *parent, struct sgfs_node *node)
{
	KASSERT(parent != NULL, ("%s(): parent is NULL", __func__));
	KASSERT(parent->fsinfo != NULL,
	    ("%s(): parent has no fsinfo", __func__));
	KASSERT(parent->type == sgfstype_dir ||
	    parent->type == sgfstype_root,
	    ("%s(): parent is not a directory", __func__));

	node->fsinfo = parent->fsinfo;
	node->parent = parent;
	LIST_INIT(&node->children);
	LIST_INIT(&node->vnodes);
	LIST_INSERT_HEAD(&parent->children, node, sibling);
	sgnode_addref(parent);
	return (0);
}

static struct sgfs_node *
sgfs_create_node(const char *name, int namelen, struct ucred *cred, int mode,
	int nodetype)
{
	struct sgfs_node *node;

	node = sgnode_alloc();
	strncpy(node->name, name, namelen);
	node->type = nodetype;
	node->refcount = 1;
	vfs_timestamp(&node->birth);
	node->ctime = node->atime = node->mtime = node->birth;
	node->uid = cred->cr_uid;
	node->gid = cred->cr_gid;
	node->mode = mode;
	return (node);
}

#if 0
/*
 * Create a file
 */
static struct sgfs_node *
sgfs_create_file(struct sgfs_node *parent, const char *name, int namelen,
	struct ucred *cred, int mode)
{
	struct sgfs_node *node;

	node = sgfs_create_node(name, namelen, cred, mode, sgfstype_file);
	if (sgfs_add_node(parent, node) != 0) {
		sgnode_free(node);
		return (NULL);
	}
	return (node);
}
#endif

/*
 * Add . and .. to a directory
 */
static int
sgfs_fixup_dir(struct sgfs_node *parent, struct ucred *cred)
{
	struct sgfs_node *dir;

	dir = sgfs_create_node(".", 1, cred, 0755, sgfstype_this);
	if (sgfs_add_node(parent, dir) != 0) {
		sgnode_free(dir);
		return (-1);
	}

	dir = sgfs_create_node("..", 2, cred, 0755, sgfstype_parent);
	if (sgfs_add_node(parent, dir) != 0) {
		sgnode_free(dir);
		return (-1);
	}

	return (0);
}

/*
 * Create a directory
 */
static struct sgfs_node *
sgfs_create_dir(struct sgfs_node *parent, const char *name, int namelen,
	struct ucred *cred, int mode)
{
	struct sgfs_node *node;

	node = sgfs_create_node(name, namelen, cred, mode, sgfstype_dir);
	if (sgfs_add_node(parent, node) != 0) {
		sgnode_free(node);
		return (NULL);
	}

	if (sgfs_fixup_dir(node, cred) != 0) {
		sgfs_destroy(node);
		return (NULL);
	}
	return (node);
}

/*
 * Destroy a node or a tree of nodes
 */
static int
sgfs_destroy(struct sgfs_node *node)
{
	struct sgfs_node *parent;

	KASSERT(node != NULL,
	    ("%s(): node is NULL", __func__));
	KASSERT(node->fsinfo != NULL,
	    ("%s(): node has no mn_info", __func__));

	/* destroy children */
	if (node->type == sgfstype_dir || node->type == sgfstype_root)
		while (! LIST_EMPTY(&node->children))
			sgfs_destroy(LIST_FIRST(&node->children));

	/* unlink from parent */
	if ((parent = node->parent) != NULL) {
		KASSERT(parent->fsinfo == node->fsinfo,
		    ("%s(): parent has different fsinfo", __func__));
		LIST_REMOVE(node, sibling);
	}

	if (node->fileno != 0)
		sgfs_fileno_free(node->fsinfo, node);
	if (node->data != NULL)
		sgdata_free(node->data);
	sgnode_free(node);
	return (0);
}

/*
 * Mount a sgfs instance
 */
static int
sgfs_mount(struct mount *mp)
{
	struct statfs *sbp;

	if (mp->mnt_flag & MNT_UPDATE)
		return (EOPNOTSUPP);

	mp->mnt_data = &sgfs_data;
	MNT_ILOCK(mp);
	mp->mnt_flag |= MNT_LOCAL;
	MNT_IUNLOCK(mp);
	vfs_getnewfsid(mp);

	sbp = &mp->mnt_stat;
	vfs_mountedfrom(mp, "sgfs");
	sbp->f_bsize = PAGE_SIZE;
	sbp->f_iosize = PAGE_SIZE;
	sbp->f_blocks = 1;
	sbp->f_bfree = 0;
	sbp->f_bavail = 0;
	sbp->f_files = 1;
	sbp->f_ffree = 0;
	return (0);
}

/*
 * Unmount a sgfs instance
 */
static int
sgfs_unmount(struct mount *mp, int mntflags)
{
	int error;

	error = vflush(mp, 0, (mntflags & MNT_FORCE) ?  FORCECLOSE : 0,
	    curthread);
	return (error);
}

/*
 * Return a root vnode
 */
static int
sgfs_root(struct mount *mp, int flags, struct vnode **vpp)
{
	struct sgfs_info *sgfs;
	int ret;

	sgfs = VFSTOSGFS(mp);
	ret = sgfs_allocv(mp, vpp, sgfs->root);
	return (ret);
}

/*
 * Return filesystem stats
 */
static int
sgfs_statfs(struct mount *mp, struct statfs *sbp)
{
	/* XXX update statistics */
	return (0);
}

/*
 * Initialize a sgfs instance
 */
static int
sgfs_init(struct vfsconf *vfc)
{
	struct sgfs_node *root;
	struct sgfs_info *fs;

	sgnode_zone = uma_zcreate("sgnode", sizeof(struct sgfs_node),
		NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	sgvdata_zone = uma_zcreate("sgvdata",
		sizeof(struct sgfs_vdata), NULL, NULL, NULL,
		NULL, UMA_ALIGN_PTR, 0);
	fs = &sgfs_data;
	sx_init(&fs->lock, "sgfs lock");
	/* set up the root diretory */
	root = sgfs_create_node("/", 1, curthread->td_ucred, 01777,
		sgfstype_root);
	root->fsinfo = fs;
	LIST_INIT(&root->children);
	LIST_INIT(&root->vnodes);
	fs->root = root;
	sgfs_fileno_init(fs);
	sgfs_fileno_alloc(fs, root);
	sgfs_fixup_dir(root, curthread->td_ucred);
	return (0);
}

/*
 * Destroy a sgfs instance
 */
static int
sgfs_uninit(struct vfsconf *vfc)
{
	struct sgfs_info *fs;

	if (!unloadable)
		return (EOPNOTSUPP);
	fs = &sgfs_data;
	sgfs_destroy(fs->root);
	fs->root = NULL;
	sgfs_fileno_uninit(fs);
	sx_destroy(&fs->lock);
	uma_zdestroy(sgnode_zone);
	uma_zdestroy(sgvdata_zone);
	return (0);
}

/*
 * task routine
 */
static void
do_recycle(void *context, int pending __unused)
{
	struct vnode *vp = (struct vnode *)context;

	vrecycle(vp);
	vdrop(vp);
}

/*
 * Allocate a vnode
 */
static int
sgfs_allocv(struct mount *mp, struct vnode **vpp, struct sgfs_node *node)
{
	struct sgfs_vdata *vd;
	struct sgfs_info  *fs;
	struct vnode *newvpp;
	int error;

	fs = node->fsinfo;
	*vpp = NULL;
	sx_xlock(&fs->lock);
	LIST_FOREACH(vd, &node->vnodes, link) {
		if (vd->vnode->v_mount == mp) {
			vhold(vd->vnode);
			break;
		}
	}

	if (vd != NULL) {
found:
		*vpp = vd->vnode;
		sx_xunlock(&fs->lock);
		error = vget(*vpp, LK_RETRY | LK_EXCLUSIVE, curthread);
		vdrop(*vpp);
		return (error);
	}
	sx_xunlock(&fs->lock);

	error = getnewvnode("sgfs", mp, &sgfs_vnodeops, &newvpp);
	if (error)
		return (error);
	vn_lock(newvpp, LK_EXCLUSIVE | LK_RETRY);
	error = insmntque(newvpp, mp);
	if (error != 0)
		return (error);

	sx_xlock(&fs->lock);
	/*
	 * Check if it has already been allocated
	 * while we were blocked.
	 */
	LIST_FOREACH(vd, &node->vnodes, link) {
		if (vd->vnode->v_mount == mp) {
			vhold(vd->vnode);
			sx_xunlock(&fs->lock);

			vgone(newvpp);
			vput(newvpp);
			goto found;
		}
	}

	*vpp = newvpp;

	vd = uma_zalloc(sgvdata_zone, M_WAITOK);
	(*vpp)->v_data = vd;
	vd->vnode = *vpp;
	vd->node = node;
	TASK_INIT(&vd->task, 0, do_recycle, *vpp);
	LIST_INSERT_HEAD(&node->vnodes, vd, link);
	sgnode_addref(node);
	switch (node->type) {
	case sgfstype_root:
		(*vpp)->v_vflag = VV_ROOT;
		/* fall through */
	case sgfstype_dir:
	case sgfstype_this:
	case sgfstype_parent:
		(*vpp)->v_type = VDIR;
		break;
	case sgfstype_file:
		(*vpp)->v_type = VREG;
		break;
	case sgfstype_symlink:
		(*vpp)->v_type = VLNK;
		break;
	case sgfstype_none:
		KASSERT(0, ("sgfs_allocf called for null node\n"));
	default:
		panic("%s has unexpected type: %d", node->name, node->type);
	}
	sx_xunlock(&fs->lock);
	return (0);
}

/* 
 * Search a directory entry
 */
static struct sgfs_node *
sgfs_search(struct sgfs_node *parent, const char *name, int len)
{
	struct sgfs_node *node;

	sx_assert(&parent->fsinfo->lock, SX_LOCKED);
	LIST_FOREACH(node, &parent->children, sibling) {
		if (strncmp(node->name, name, len) == 0 &&
		    node->name[len] == '\0')
			return (node);
	}
	return (NULL);
}

/*
 * Look up a file or directory.
 */
static int
sgfs_lookupx(struct vop_cachedlookup_args *ap)
{
	struct componentname *cnp;
	struct vnode *dvp, **vpp;
	struct sgfs_node *pd;
	struct sgfs_node *pn;
	struct sgfs_info *fs;
	int nameiop, flags, error, namelen;
	char *pname;
	struct thread *td;

	cnp = ap->a_cnp;
	vpp = ap->a_vpp;
	dvp = ap->a_dvp;
	pname = cnp->cn_nameptr;
	namelen = cnp->cn_namelen;
	td = cnp->cn_thread;
	flags = cnp->cn_flags;
	nameiop = cnp->cn_nameiop;
	pd = VTON(dvp);
	pn = NULL;
	fs = pd->fsinfo;
	*vpp = NULLVP;

	if (dvp->v_type != VDIR)
		return (ENOTDIR);

	error = VOP_ACCESS(dvp, VEXEC, cnp->cn_cred, cnp->cn_thread);
	if (error)
		return (error);

	/* shortcut: check if the name is too long */
	if (cnp->cn_namelen >= SGFS_NAMELEN)
		return (ENOENT);

	/* self */
	if (namelen == 1 && pname[0] == '.') {
		if ((flags & ISLASTCN) && nameiop != LOOKUP)
			return (EINVAL);
		pn = pd;
		*vpp = dvp;
		VREF(dvp);
		return (0);
	}

	/* parent */
	if (cnp->cn_flags & ISDOTDOT) {
		if (dvp->v_vflag & VV_ROOT)
			return (EIO);
		if ((flags & ISLASTCN) && nameiop != LOOKUP)
			return (EINVAL);
		VOP_UNLOCK(dvp, 0);
		KASSERT(pd->parent, ("non-root directory has no parent"));
		pn = pd->parent;
		error = sgfs_allocv(dvp->v_mount, vpp, pn);
		vn_lock(dvp, LK_EXCLUSIVE | LK_RETRY);
		return (error);
	}

	/* named node */
	sx_xlock(&fs->lock);
	pn = sgfs_search(pd, pname, namelen);
	if (pn != NULL)
		sgnode_addref(pn);
	sx_xunlock(&fs->lock);
	
	/* found */
	if (pn != NULL) {
		/* DELETE */
		if (nameiop == DELETE && (flags & ISLASTCN)) {
			error = VOP_ACCESS(dvp, VWRITE, cnp->cn_cred, td);
			if (error) {
				sgnode_release(pn);
				return (error);
			}
			if (*vpp == dvp) {
				VREF(dvp);
				*vpp = dvp;
				sgnode_release(pn);
				return (0);
			}
		}

		/* allocate vnode */
		error = sgfs_allocv(dvp->v_mount, vpp, pn);
		sgnode_release(pn);
		if (error == 0 && cnp->cn_flags & MAKEENTRY)
			cache_enter(dvp, *vpp, cnp);
		return (error);
	}
	
	/* not found */

	/* will create a new entry in the directory ? */
	if ((nameiop == CREATE || nameiop == RENAME) && (flags & LOCKPARENT)
	    && (flags & ISLASTCN)) {
		error = VOP_ACCESS(dvp, VWRITE, cnp->cn_cred, td);
		if (error)
			return (error);
		cnp->cn_flags |= SAVENAME;
		return (EJUSTRETURN);
	}
	return (ENOENT);
}

#if 0
struct vop_lookup_args {
	struct vop_generic_args a_gen;
	struct vnode *a_dvp;
	struct vnode **a_vpp;
	struct componentname *a_cnp;
};
#endif

/*
 * vnode lookup operation
 */
static int
sgfs_lookup(struct vop_cachedlookup_args *ap)
{
	int rc;

	rc = sgfs_lookupx(ap);
	return (rc);
}

#if 0
struct vop_inactive_args {
	struct vnode *a_vp;
	struct thread *a_td;
};
#endif

static int
sgfs_inactive(struct vop_inactive_args *ap)
{
	struct sgfs_node *node = VTON(ap->a_vp);

	if (node->deleted)
		vrecycle(ap->a_vp);
	return (0);
}

#if 0
struct vop_reclaim_args {
	struct vop_generic_args a_gen;
	struct vnode *a_vp;
	struct thread *a_td;
};
#endif

static int
sgfs_reclaim(struct vop_reclaim_args *ap)
{
	struct sgfs_info *fs = VFSTOSGFS(ap->a_vp->v_mount);
	struct vnode *vp = ap->a_vp;
	struct sgfs_node *pn;
	struct sgfs_vdata *vd;

	vd = vp->v_data;
	pn = vd->node;
	sx_xlock(&fs->lock);
	vp->v_data = NULL;
	LIST_REMOVE(vd, link);
	uma_zfree(sgvdata_zone, vd);
	sgnode_release(pn);
	sx_xunlock(&fs->lock);
	return (0);
}

#if 0
struct vop_open_args {
	struct vop_generic_args a_gen;
	struct vnode *a_vp;
	int a_mode;
	struct ucred *a_cred;
	struct thread *a_td;
	struct file *a_fp;
};
#endif

static int
sgfs_open(struct vop_open_args *ap)
{
	return (0);
}

#if 0
struct vop_close_args {
	struct vop_generic_args a_gen;
	struct vnode *a_vp;
	int a_fflag;
	struct ucred *a_cred;
	struct thread *a_td;
};
#endif

static int
sgfs_close(struct vop_close_args *ap)
{
	return (0);
}

#if 0
struct vop_access_args {
	struct vop_generic_args a_gen;
	struct vnode *a_vp;
	accmode_t a_accmode;
	struct ucred *a_cred;
	struct thread *a_td;
};
#endif

/*
 * Verify permissions
 */
static int
sgfs_access(struct vop_access_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vattr vattr;
	int error;

	error = VOP_GETATTR(vp, &vattr, ap->a_cred);
	if (error)
		return (error);
	error = vaccess(vp->v_type, vattr.va_mode, vattr.va_uid,
	    vattr.va_gid, ap->a_accmode, ap->a_cred, NULL);
	return (error);
}

#if 0
struct vop_getattr_args {
	struct vop_generic_args a_gen;
	struct vnode *a_vp;
	struct vattr *a_vap;
	struct ucred *a_cred;
};
#endif

/*
 * Get file attributes
 */
static int
sgfs_getattr(struct vop_getattr_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct sgfs_node *pn = VTON(vp);
	struct vattr *vap = ap->a_vap;
	int error = 0;

	vap->va_type = vp->v_type;
	vap->va_mode = pn->mode;
	vap->va_nlink = 1;
	vap->va_uid = pn->uid;
	vap->va_gid = pn->gid;
	vap->va_fsid = vp->v_mount->mnt_stat.f_fsid.val[0];
	vap->va_fileid = pn->fileno;
	vap->va_size = 0;
	vap->va_blocksize = PAGE_SIZE;
	vap->va_bytes = vap->va_size = 0;
	vap->va_atime = pn->atime;
	vap->va_mtime = pn->mtime;
	vap->va_ctime = pn->ctime;
	vap->va_birthtime = pn->birth;
	vap->va_gen = 0;
	vap->va_flags = 0;
	vap->va_rdev = NODEV;
	vap->va_bytes = 0;
	vap->va_filerev = 0;
	return (error);
}

#if 0
struct vop_setattr_args {
	struct vop_generic_args a_gen;
	struct vnode *a_vp;
	struct vattr *a_vap;
	struct ucred *a_cred;
};
#endif
/*
 * Set attributes
 */
static int
sgfs_setattr(struct vop_setattr_args *ap)
{
	struct sgfs_node *pn;
	struct vattr *vap;
	struct vnode *vp;
	struct thread *td;
	int c, error;
	uid_t uid;
	gid_t gid;

	td = curthread;
	vap = ap->a_vap;
	vp = ap->a_vp;
	if ((vap->va_type != VNON) ||
	    (vap->va_nlink != VNOVAL) ||
	    (vap->va_fsid != VNOVAL) ||
	    (vap->va_fileid != VNOVAL) ||
	    (vap->va_blocksize != VNOVAL) ||
	    (vap->va_flags != VNOVAL && vap->va_flags != 0) ||
	    (vap->va_rdev != VNOVAL) ||
	    ((int)vap->va_bytes != VNOVAL) ||
	    (vap->va_gen != VNOVAL)) {
		return (EINVAL);
	}

	pn = VTON(vp);

	error = c = 0;
	if (vap->va_uid == (uid_t)VNOVAL)
		uid = pn->uid;
	else
		uid = vap->va_uid;
	if (vap->va_gid == (gid_t)VNOVAL)
		gid = pn->gid;
	else
		gid = vap->va_gid;

	if (uid != pn->uid || gid != pn->gid) {
		/*
		 * To modify the ownership of a file, must possess VADMIN
		 * for that file.
		 */
		if ((error = VOP_ACCESS(vp, VADMIN, ap->a_cred, td)))
			return (error);

		/*
		 * XXXRW: Why is there a privilege check here: shouldn't the
		 * check in VOP_ACCESS() be enough?  Also, are the group bits
		 * below definitely right?
		 */
		if (((ap->a_cred->cr_uid != pn->uid) || uid != pn->uid ||
		    (gid != pn->gid && !groupmember(gid, ap->a_cred))) &&
		    (error = priv_check(td,  PRIV_VFS_CHOWN)) != 0)
			return (error);
		pn->uid = uid;
		pn->gid = gid;
		c = 1;
	}

	if (vap->va_mode != (mode_t)VNOVAL) {
		if ((ap->a_cred->cr_uid != pn->uid) &&
		    (error = priv_check(td, PRIV_VFS_CHOWN)))
			return (error);
		pn->mode = vap->va_mode;
		c = 1;
	}

	if (vap->va_atime.tv_sec != VNOVAL || vap->va_mtime.tv_sec != VNOVAL) {
		/* See the comment in ufs_vnops::ufs_setattr(). */
		if ((error = VOP_ACCESS(vp, VADMIN, ap->a_cred, td)) &&
		    ((vap->va_vaflags & VA_UTIMES_NULL) == 0 ||
		    (error = VOP_ACCESS(vp, VWRITE, ap->a_cred, td))))
			return (error);
		if (vap->va_atime.tv_sec != VNOVAL) {
			pn->atime = vap->va_atime;
		}
		if (vap->va_mtime.tv_sec != VNOVAL) {
			pn->mtime = vap->va_mtime;
		}
		c = 1;
	}
	if (c) {
		vfs_timestamp(&pn->ctime);
	}
	return (0);
}

#if 0
struct vop_read_args {
	struct vop_generic_args a_gen;
	struct vnode *a_vp;
	struct uio *a_uio;
	int a_ioflag;
	struct ucred *a_cred;
};
#endif

/*
 * Read from a file
 */
static int
sgfs_read(struct vop_read_args *ap)
{
	char buf[80];
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct sgfs_node *pn;
	int len, error;

	if (vp->v_type != VREG)
		return (EINVAL);

	pn = VTON(vp);
	snprintf(buf, sizeof(buf), "hello world\n");
	buf[sizeof(buf)-1] = '\0';
	len = strlen(buf);
	error = uiomove_frombuf(buf, len, uio);
	return (error);
}

#if 0
struct vop_readdir_args {
	struct vop_generic_args a_gen;
	struct vnode *a_vp;
	struct uio *a_uio;
	struct ucred *a_cred;
	int *a_eofflag;
	int *a_ncookies;
	u_long **a_cookies;
};
#endif

/*
 * Return directory entries.
 */
static int
sgfs_readdir(struct vop_readdir_args *ap)
{
	struct vnode *vp;
	struct sgfs_info *fs;
	struct sgfs_node *pd;
	struct sgfs_node *pn;
	struct dirent entry;
	struct uio *uio;
	int *tmp_ncookies = NULL;
	off_t offset;
	int error, i;

	vp = ap->a_vp;
	fs = VFSTOSGFS(vp->v_mount);
	pd = VTON(vp);
	uio = ap->a_uio;

	if (vp->v_type != VDIR)
		return (ENOTDIR);

	if (uio->uio_offset < 0)
		return (EINVAL);

	if (ap->a_ncookies != NULL) {
		tmp_ncookies = ap->a_ncookies;
		*ap->a_ncookies = 0;
		ap->a_ncookies = NULL;
        }

	error = 0;
	offset = 0;

	sx_xlock(&fs->lock);

	LIST_FOREACH(pn, &pd->children, sibling) {
		entry.d_reclen = sizeof(entry);
		if (!pn->fileno)
			sgfs_fileno_alloc(fs, pn);
		entry.d_fileno = pn->fileno;
		for (i = 0; i < SGFS_NAMELEN - 1 && pn->name[i] != '\0'; ++i)
			entry.d_name[i] = pn->name[i];
		entry.d_name[i] = 0;
		entry.d_namlen = i;
		switch (pn->type) {
		case sgfstype_root:
		case sgfstype_dir:
		case sgfstype_this:
		case sgfstype_parent:
			entry.d_type = DT_DIR;
			break;
		case sgfstype_file:
			entry.d_type = DT_REG;
			break;
		case sgfstype_symlink:
			entry.d_type = DT_LNK;
			break;
		default:
			panic("%s has unexpected node type: %d", pn->name,
				pn->type);
		}
		if (entry.d_reclen > uio->uio_resid)
                        break;
		if (offset >= uio->uio_offset) {
			error = vfs_read_dirent(ap, &entry, offset);
                        if (error)
                                break;
                }
                offset += entry.d_reclen;
	}
	sx_xunlock(&fs->lock);

	uio->uio_offset = offset;

	if (tmp_ncookies != NULL)
		ap->a_ncookies = tmp_ncookies;

	return (error);
}

#if 0
struct vop_mkdir_args {
	struct vnode *a_dvp;
	struvt vnode **a_vpp;
	struvt componentname *a_cnp;
	struct vattr *a_vap;
};
#endif

/*
 * Create a directory.
 */
static int
sgfs_mkdir(struct vop_mkdir_args *ap)
{
	struct sgfs_info *fs = VFSTOSGFS(ap->a_dvp->v_mount);
	struct componentname *cnp = ap->a_cnp;
	struct sgfs_node *pd = VTON(ap->a_dvp);
	struct sgfs_node *pn;
	int error;

	if (pd->type != sgfstype_root && pd->type != sgfstype_dir)
		return (ENOTDIR);
	sx_xlock(&fs->lock);
	if ((cnp->cn_flags & HASBUF) == 0)
		panic("%s: no name", __func__);
	pn = sgfs_create_dir(pd, cnp->cn_nameptr, cnp->cn_namelen,
		ap->a_cnp->cn_cred, ap->a_vap->va_mode);
	if (pn != NULL)
		sgnode_addref(pn);
	sx_xunlock(&fs->lock);
	if (pn == NULL) {
		error = ENOSPC;
	} else {
		error = sgfs_allocv(ap->a_dvp->v_mount, ap->a_vpp, pn);
		sgnode_release(pn);
	}
	return (error);
}

#if 0
struct vop_rmdir_args {
	struct vnode *a_dvp;
	struct vnode *a_vp;
	struct componentname *a_cnp;
};
#endif

/*
 * Remove a directory.
 */
static int
sgfs_rmdir(struct vop_rmdir_args *ap)
{
	struct sgfs_info *fs = VFSTOSGFS(ap->a_dvp->v_mount);
	struct sgfs_node *pn = VTON(ap->a_vp);
	struct sgfs_node *pt;

	if (pn->type != sgfstype_dir)
		return (ENOTDIR);

	sx_xlock(&fs->lock);
	if (pn->deleted) {
		sx_xunlock(&fs->lock);
		return (ENOENT);
	}

	pt = LIST_FIRST(&pn->children);
	pt = LIST_NEXT(pt, sibling);
	pt = LIST_NEXT(pt, sibling);
	if (pt != NULL) {
		sx_xunlock(&fs->lock);
		return (ENOTEMPTY);
	}
	pt = pn->parent;
	pn->parent = NULL;
	pn->deleted = 1;
	LIST_REMOVE(pn, sibling);
	sgnode_release(pn);
	sgnode_release(pt);
	sx_xunlock(&fs->lock);
	cache_purge(ap->a_vp);
	return (0);
}

static struct vop_vector sgfs_vnodeops = {
	.vop_default 		= &default_vnodeops,
	.vop_access		= sgfs_access,
	.vop_cachedlookup	= sgfs_lookup,
	.vop_lookup		= vfs_cache_lookup,
	.vop_reclaim		= sgfs_reclaim,
	.vop_create		= VOP_EOPNOTSUPP,
	.vop_remove		= VOP_EOPNOTSUPP,
	.vop_inactive		= sgfs_inactive,
	.vop_open		= sgfs_open,
	.vop_close		= sgfs_close,
	.vop_getattr		= sgfs_getattr,
	.vop_setattr		= sgfs_setattr,
	.vop_read		= sgfs_read,
	.vop_write		= VOP_EOPNOTSUPP,
	.vop_readdir		= sgfs_readdir,
	.vop_mkdir		= sgfs_mkdir,
	.vop_rmdir		= sgfs_rmdir
};

static struct vfsops sgfs_vfsops = {
	.vfs_init 		= sgfs_init,
	.vfs_uninit		= sgfs_uninit,
	.vfs_mount		= sgfs_mount,
	.vfs_unmount		= sgfs_unmount,
	.vfs_root		= sgfs_root,
	.vfs_statfs		= sgfs_statfs,
};

VFS_SET(sgfs_vfsops, sgfs, VFCF_SYNTHETIC);
