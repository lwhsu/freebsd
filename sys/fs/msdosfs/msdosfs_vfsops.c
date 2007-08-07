/* $FreeBSD$ */
/*	$NetBSD: msdosfs_vfsops.c,v 1.51 1997/11/17 15:36:58 ws Exp $	*/

/*-
 * Copyright (C) 1994, 1995, 1997 Wolfgang Solfrank.
 * Copyright (C) 1994, 1995, 1997 TooLs GmbH.
 * All rights reserved.
 * Original code by Paul Popelka (paulp@uts.amdahl.com) (see below).
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by TooLs GmbH.
 * 4. The name of TooLs GmbH may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY TOOLS GMBH ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL TOOLS GMBH BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*-
 * Written by Paul Popelka (paulp@uts.amdahl.com)
 *
 * You can do anything you want with this software, just don't say you wrote
 * it, and don't remove this notice.
 *
 * This software is provided "as is".
 *
 * The author supplies this software to be publicly redistributed on the
 * understanding that the author is not responsible for the correct
 * functioning of this software in any circumstances and is not liable for
 * any damages caused by this software.
 *
 * October 1992
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/namei.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/buf.h>
#include <sys/malloc.h>
#include <sys/stat.h> 				/* defines ALLPERMS */
#include <sys/iconv.h>
#include <sys/mutex.h>

#include <fs/msdosfs/bpb.h>
#include <fs/msdosfs/bootsect.h>
#include <fs/msdosfs/msdosfsmount.h>
#include <fs/msdosfs/direntry.h>
#include <fs/msdosfs/denode.h>
#include <fs/msdosfs/fat.h>

#include <geom/geom.h>
#include <geom/geom_vfs.h>

/* List of mount options we support */
static const char *msdosfs_opts[] = {
	"from",
	"atime", "export", "force", "sync",
	"uid", "gid", "mask", "dirmask",
	"shortname", "shortnames", "longname", "longnames", "nowin95", "win95",
	"kiconv", "cs_win", "cs_dos", "cs_local", "large",
	NULL
};

#if 1 /*def PC98*/
/*
 * XXX - The boot signature formatted by NEC PC-98 DOS looks like a
 *       garbage or a random value :-{
 *       If you want to use that broken-signatured media, define the
 *       following symbol even though PC/AT.
 *       (ex. mount PC-98 DOS formatted FD on PC/AT)
 */
#define	MSDOSFS_NOCHECKSIG
#endif

MALLOC_DEFINE(M_MSDOSFSMNT, "msdosfs_mount", "MSDOSFS mount structure");
static MALLOC_DEFINE(M_MSDOSFSFAT, "msdosfs_fat", "MSDOSFS file allocation table");

struct iconv_functions *msdosfs_iconv = NULL;

static int	update_mp(struct mount *mp, struct thread *td);
static int	mountmsdosfs(struct vnode *devvp, struct mount *mp,
		    struct thread *td);
static vfs_fhtovp_t	msdosfs_fhtovp;
static vfs_mount_t	msdosfs_mount;
static vfs_root_t	msdosfs_root;
static vfs_statfs_t	msdosfs_statfs;
static vfs_sync_t	msdosfs_sync;
static vfs_unmount_t	msdosfs_unmount;

/* Maximum length of a character set name (arbitrary). */
#define	MAXCSLEN	64

static int
update_mp(struct mount *mp, struct thread *td)
{
	struct msdosfsmount *pmp = VFSTOMSDOSFS(mp);
	void *dos, *win, *local;
	int error, v;

	if (!vfs_getopt(mp->mnt_optnew, "kiconv", NULL, NULL)) {
		if (msdosfs_iconv != NULL) {
			error = vfs_getopt(mp->mnt_optnew,
			    "cs_win", &win, NULL);
			if (!error)
				error = vfs_getopt(mp->mnt_optnew,
				    "cs_local", &local, NULL);
			if (!error)
				error = vfs_getopt(mp->mnt_optnew,
				    "cs_dos", &dos, NULL);
			if (!error) {
				msdosfs_iconv->open(win, local, &pmp->pm_u2w);
				msdosfs_iconv->open(local, win, &pmp->pm_w2u);
				msdosfs_iconv->open(dos, local, &pmp->pm_u2d);
				msdosfs_iconv->open(local, dos, &pmp->pm_d2u);
			}
			if (error != 0)
				return (error);
		} else {
			pmp->pm_w2u = NULL;
			pmp->pm_u2w = NULL;
			pmp->pm_d2u = NULL;
			pmp->pm_u2d = NULL;
		}
	}

	if (1 == vfs_scanopt(mp->mnt_optnew, "gid", "%d", &v))
		pmp->pm_gid = v;
	if (1 == vfs_scanopt(mp->mnt_optnew, "uid", "%d", &v))
		pmp->pm_uid = v;
	if (1 == vfs_scanopt(mp->mnt_optnew, "mask", "%d", &v))
		pmp->pm_mask = v & ALLPERMS;
	if (1 == vfs_scanopt(mp->mnt_optnew, "dirmask", "%d", &v))
		pmp->pm_dirmask = v & ALLPERMS;
	vfs_flagopt(mp->mnt_optnew, "shortname",
	    &pmp->pm_flags, MSDOSFSMNT_SHORTNAME);
	vfs_flagopt(mp->mnt_optnew, "shortnames",
	    &pmp->pm_flags, MSDOSFSMNT_SHORTNAME);
	vfs_flagopt(mp->mnt_optnew, "longname",
	    &pmp->pm_flags, MSDOSFSMNT_LONGNAME);
	vfs_flagopt(mp->mnt_optnew, "longnames",
	    &pmp->pm_flags, MSDOSFSMNT_LONGNAME);
	vfs_flagopt(mp->mnt_optnew, "kiconv",
	    &pmp->pm_flags, MSDOSFSMNT_KICONV);

	if (vfs_getopt(mp->mnt_optnew, "nowin95", NULL, NULL) == 0)
		pmp->pm_flags |= MSDOSFSMNT_NOWIN95;
	else
		pmp->pm_flags &= ~MSDOSFSMNT_NOWIN95;

	if (pmp->pm_flags & MSDOSFSMNT_NOWIN95)
		pmp->pm_flags |= MSDOSFSMNT_SHORTNAME;
	else if (!(pmp->pm_flags &
	    (MSDOSFSMNT_SHORTNAME | MSDOSFSMNT_LONGNAME))) {
		struct vnode *rootvp;

		/*
		 * Try to divine whether to support Win'95 long filenames
		 */
		if (FAT32(pmp))
			pmp->pm_flags |= MSDOSFSMNT_LONGNAME;
		else {
			if ((error =
			    msdosfs_root(mp, LK_EXCLUSIVE, &rootvp, td)) != 0)
				return error;
			pmp->pm_flags |= findwin95(VTODE(rootvp))
				? MSDOSFSMNT_LONGNAME
					: MSDOSFSMNT_SHORTNAME;
			vput(rootvp);
		}
	}
	return 0;
}

static int
msdosfs_cmount(struct mntarg *ma, void *data, int flags, struct thread *td)
{
	struct msdosfs_args args;
	int error;

	if (data == NULL)
		return (EINVAL);
	error = copyin(data, &args, sizeof args);
	if (error)
		return (error);

	ma = mount_argsu(ma, "from", args.fspec, MAXPATHLEN);
	ma = mount_arg(ma, "export", &args.export, sizeof args.export);
	ma = mount_argf(ma, "uid", "%d", args.uid);
	ma = mount_argf(ma, "gid", "%d", args.gid);
	ma = mount_argf(ma, "mask", "%d", args.mask);
	ma = mount_argf(ma, "dirmask", "%d", args.dirmask);

        ma = mount_argb(ma, args.flags & MSDOSFSMNT_SHORTNAME, "noshortname");
        ma = mount_argb(ma, args.flags & MSDOSFSMNT_LONGNAME, "nolongname");
        ma = mount_argb(ma, !(args.flags & MSDOSFSMNT_NOWIN95), "nowin95");
        ma = mount_argb(ma, args.flags & MSDOSFSMNT_KICONV, "nokiconv");

        ma = mount_argsu(ma, "cs_win", args.cs_win, MAXCSLEN);
        ma = mount_argsu(ma, "cs_dos", args.cs_dos, MAXCSLEN);
        ma = mount_argsu(ma, "cs_local", args.cs_local, MAXCSLEN);

	error = kernel_mount(ma, flags);

	return (error);
}

/*
 * mp - path - addr in user space of mount point (ie /usr or whatever)
 * data - addr in user space of mount params including the name of the block
 * special file to treat as a filesystem.
 */
static int
msdosfs_mount(struct mount *mp, struct thread *td)
{
	struct vnode *devvp;	  /* vnode for blk device to mount */
	/* msdosfs specific mount control block */
	struct msdosfsmount *pmp = NULL;
	struct nameidata ndp;
	int error, flags;
	mode_t accessmode;
	char *from;

	if (vfs_filteropt(mp->mnt_optnew, msdosfs_opts))
		return (EINVAL);

	/*
	 * If updating, check whether changing from read-only to
	 * read/write; if there is no device name, that's all we do.
	 */
	if (mp->mnt_flag & MNT_UPDATE) {
		int ro_to_rw = 0;
		pmp = VFSTOMSDOSFS(mp);

		if (vfs_flagopt(mp->mnt_optnew, "export", NULL, 0)) {
			/*
			 * Forbid export requests if filesystem has
			 * MSDOSFS_LARGEFS flag set.
			 */
			if ((pmp->pm_flags & MSDOSFS_LARGEFS) != 0) {
				vfs_mount_error(mp,
				    "MSDOSFS_LARGEFS flag set, cannot export");
				return (EOPNOTSUPP);
			}
		}
		if (!(pmp->pm_flags & MSDOSFSMNT_RONLY) &&
		    vfs_flagopt(mp->mnt_optnew, "ro", NULL, 0)) {
			error = VFS_SYNC(mp, MNT_WAIT, td);
			if (error)
				return (error);
			flags = WRITECLOSE;
			if (mp->mnt_flag & MNT_FORCE)
				flags |= FORCECLOSE;
			error = vflush(mp, 0, flags, td);
			if (error)
				return (error);
			DROP_GIANT();
			g_topology_lock();
			error = g_access(pmp->pm_cp, 0, -1, 0);
			g_topology_unlock();
			PICKUP_GIANT();
			if (error)
				return (error);

			/* Now the volume is clean. Mark it. */
			error = markvoldirty(pmp, 0);
			if (error && (flags & FORCECLOSE) == 0)
				return (error);
		} else if ((pmp->pm_flags & MSDOSFSMNT_RONLY) &&
		    !vfs_flagopt(mp->mnt_optnew, "ro", NULL, 0)) {
			/*
			 * If upgrade to read-write by non-root, then verify
			 * that user has necessary permissions on the device.
			 */
			devvp = pmp->pm_devvp;
			vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY, td);
			error = VOP_ACCESS(devvp, VREAD | VWRITE,
			   td->td_ucred, td);
			if (error)
				error = priv_check(td, PRIV_VFS_MOUNT_PERM);
			if (error) {
				VOP_UNLOCK(devvp, 0, td);
				return (error);
			}
			VOP_UNLOCK(devvp, 0, td);
			DROP_GIANT();
			g_topology_lock();
			error = g_access(pmp->pm_cp, 0, 1, 0);
			g_topology_unlock();
			PICKUP_GIANT();
			if (error)
				return (error);

			ro_to_rw = 1;
		}
		vfs_flagopt(mp->mnt_optnew, "ro",
		    &pmp->pm_flags, MSDOSFSMNT_RONLY);
		vfs_flagopt(mp->mnt_optnew, "ro",
		    &mp->mnt_flag, MNT_RDONLY);

		if (ro_to_rw) {
			/* Now that the volume is modifiable, mark it dirty. */
			error = markvoldirty(pmp, 1);
			if (error)
				return (error);
		}
	}
	/*
	 * Not an update, or updating the name: look up the name
	 * and verify that it refers to a sensible disk device.
	 */
	if (vfs_getopt(mp->mnt_optnew, "from", (void **)&from, NULL))
		return (EINVAL);
	NDINIT(&ndp, LOOKUP, FOLLOW | LOCKLEAF, UIO_SYSSPACE, from, td);
	error = namei(&ndp);
	if (error)
		return (error);
	devvp = ndp.ni_vp;
	NDFREE(&ndp, NDF_ONLY_PNBUF);

	if (!vn_isdisk(devvp, &error)) {
		vput(devvp);
		return (error);
	}
	/*
	 * If mount by non-root, then verify that user has necessary
	 * permissions on the device.
	 */
	accessmode = VREAD;
	if ((mp->mnt_flag & MNT_RDONLY) == 0)
		accessmode |= VWRITE;
	error = VOP_ACCESS(devvp, accessmode, td->td_ucred, td);
	if (error)
		error = priv_check(td, PRIV_VFS_MOUNT_PERM);
	if (error) {
		vput(devvp);
		return (error);
	}
	if ((mp->mnt_flag & MNT_UPDATE) == 0) {
		error = mountmsdosfs(devvp, mp, td);
#ifdef MSDOSFS_DEBUG		/* only needed for the printf below */
		pmp = VFSTOMSDOSFS(mp);
#endif
	} else {
		if (devvp != pmp->pm_devvp)
			error = EINVAL;	/* XXX needs translation */
		else
			vput(devvp);
	}
	if (error) {
		vrele(devvp);
		return (error);
	}

	error = update_mp(mp, td);
	if (error) {
		if ((mp->mnt_flag & MNT_UPDATE) == 0)
			msdosfs_unmount(mp, MNT_FORCE, td);
		return error;
	}
	
	vfs_mountedfrom(mp, from);
#ifdef MSDOSFS_DEBUG
	printf("msdosfs_mount(): mp %p, pmp %p, inusemap %p\n", mp, pmp, pmp->pm_inusemap);
#endif
	return (0);
}

static int
mountmsdosfs(struct vnode *devvp, struct mount *mp, struct thread *td)
{
	struct msdosfsmount *pmp;
	struct buf *bp;
	struct cdev *dev = devvp->v_rdev;
	union bootsector *bsp;
	struct byte_bpb33 *b33;
	struct byte_bpb50 *b50;
	struct byte_bpb710 *b710;
	u_int8_t SecPerClust;
	u_long clusters;
	int	ronly, error;
	struct g_consumer *cp;
	struct bufobj *bo;

	ronly = (mp->mnt_flag & MNT_RDONLY) != 0;
	/* XXX: use VOP_ACCESS to check FS perms */
	DROP_GIANT();
	g_topology_lock();
	error = g_vfs_open(devvp, &cp, "msdosfs", ronly ? 0 : 1);
	g_topology_unlock();
	PICKUP_GIANT();
	VOP_UNLOCK(devvp, 0, td);
	if (error)
		return (error);

	bo = &devvp->v_bufobj;
	bp  = NULL; /* both used in error_exit */
	pmp = NULL;

	/*
	 * Read the boot sector of the filesystem, and then check the
	 * boot signature.  If not a dos boot sector then error out.
	 *
	 * NOTE: 8192 is a magic size that works for ffs.
	 */
	error = bread(devvp, 0, 8192, NOCRED, &bp);
	if (error)
		goto error_exit;
	bp->b_flags |= B_AGE;
	bsp = (union bootsector *)bp->b_data;
	b33 = (struct byte_bpb33 *)bsp->bs33.bsBPB;
	b50 = (struct byte_bpb50 *)bsp->bs50.bsBPB;
	b710 = (struct byte_bpb710 *)bsp->bs710.bsBPB;

#ifndef MSDOSFS_NOCHECKSIG
	if (bsp->bs50.bsBootSectSig0 != BOOTSIG0
	    || bsp->bs50.bsBootSectSig1 != BOOTSIG1) {
		error = EINVAL;
		goto error_exit;
	}
#endif

	pmp = malloc(sizeof *pmp, M_MSDOSFSMNT, M_WAITOK | M_ZERO);
	pmp->pm_mountp = mp;
	pmp->pm_cp = cp;
	pmp->pm_bo = bo;

	/*
	 * Initialize ownerships and permissions, since nothing else will
	 * initialize them iff we are mounting root.
	 */
	pmp->pm_uid = UID_ROOT;
	pmp->pm_gid = GID_WHEEL;
	pmp->pm_mask = pmp->pm_dirmask = S_IXUSR | S_IXGRP | S_IXOTH |
	    S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR;

	/*
	 * Experimental support for large MS-DOS filesystems.
	 * WARNING: This uses at least 32 bytes of kernel memory (which is not
	 * reclaimed until the FS is unmounted) for each file on disk to map
	 * between the 32-bit inode numbers used by VFS and the 64-bit
	 * pseudo-inode numbers used internally by msdosfs. This is only
	 * safe to use in certain controlled situations (e.g. read-only FS
	 * with less than 1 million files).
	 * Since the mappings do not persist across unmounts (or reboots), these
	 * filesystems are not suitable for exporting through NFS, or any other
	 * application that requires fixed inode numbers.
	 */
	vfs_flagopt(mp->mnt_optnew, "large", &pmp->pm_flags,
	  MSDOSFS_LARGEFS);

	/*
	 * Compute several useful quantities from the bpb in the
	 * bootsector.  Copy in the dos 5 variant of the bpb then fix up
	 * the fields that are different between dos 5 and dos 3.3.
	 */
	SecPerClust = b50->bpbSecPerClust;
	pmp->pm_BytesPerSec = getushort(b50->bpbBytesPerSec);
	if (pmp->pm_BytesPerSec < DEV_BSIZE) {
		error = EINVAL;
		goto error_exit;
	}
	pmp->pm_ResSectors = getushort(b50->bpbResSectors);
	pmp->pm_FATs = b50->bpbFATs;
	pmp->pm_RootDirEnts = getushort(b50->bpbRootDirEnts);
	pmp->pm_Sectors = getushort(b50->bpbSectors);
	pmp->pm_FATsecs = getushort(b50->bpbFATsecs);
	pmp->pm_SecPerTrack = getushort(b50->bpbSecPerTrack);
	pmp->pm_Heads = getushort(b50->bpbHeads);
	pmp->pm_Media = b50->bpbMedia;

	/* calculate the ratio of sector size to DEV_BSIZE */
	pmp->pm_BlkPerSec = pmp->pm_BytesPerSec / DEV_BSIZE;

	/* XXX - We should probably check more values here */
	if (!pmp->pm_BytesPerSec || !SecPerClust
		|| !pmp->pm_Heads
#ifdef PC98
    		|| !pmp->pm_SecPerTrack || pmp->pm_SecPerTrack > 255) {
#else
		|| !pmp->pm_SecPerTrack || pmp->pm_SecPerTrack > 63) {
#endif
		error = EINVAL;
		goto error_exit;
	}

	if (pmp->pm_Sectors == 0) {
		pmp->pm_HiddenSects = getulong(b50->bpbHiddenSecs);
		pmp->pm_HugeSectors = getulong(b50->bpbHugeSectors);
	} else {
		pmp->pm_HiddenSects = getushort(b33->bpbHiddenSecs);
		pmp->pm_HugeSectors = pmp->pm_Sectors;
	}
	if (!(pmp->pm_flags & MSDOSFS_LARGEFS)) {
		if (pmp->pm_HugeSectors > 0xffffffff / 
		    (pmp->pm_BytesPerSec / sizeof(struct direntry)) + 1) {
			/*
			 * We cannot deal currently with this size of disk
			 * due to fileid limitations (see msdosfs_getattr and
			 * msdosfs_readdir)
			 */
			error = EINVAL;
			vfs_mount_error(mp,
			    "Disk too big, try '-o large' mount option");
			goto error_exit;
		}
	}

	if (pmp->pm_RootDirEnts == 0) {
		if (pmp->pm_Sectors
		    || pmp->pm_FATsecs
		    || getushort(b710->bpbFSVers)) {
			error = EINVAL;
			printf("mountmsdosfs(): bad FAT32 filesystem\n");
			goto error_exit;
		}
		pmp->pm_fatmask = FAT32_MASK;
		pmp->pm_fatmult = 4;
		pmp->pm_fatdiv = 1;
		pmp->pm_FATsecs = getulong(b710->bpbBigFATsecs);
		if (getushort(b710->bpbExtFlags) & FATMIRROR)
			pmp->pm_curfat = getushort(b710->bpbExtFlags) & FATNUM;
		else
			pmp->pm_flags |= MSDOSFS_FATMIRROR;
	} else
		pmp->pm_flags |= MSDOSFS_FATMIRROR;

	/*
	 * Check a few values (could do some more):
	 * - logical sector size: power of 2, >= block size
	 * - sectors per cluster: power of 2, >= 1
	 * - number of sectors:   >= 1, <= size of partition
	 * - number of FAT sectors: >= 1
	 */
	if ( (SecPerClust == 0)
	  || (SecPerClust & (SecPerClust - 1))
	  || (pmp->pm_BytesPerSec < DEV_BSIZE)
	  || (pmp->pm_BytesPerSec & (pmp->pm_BytesPerSec - 1))
	  || (pmp->pm_HugeSectors == 0)
	  || (pmp->pm_FATsecs == 0)
	) {
		error = EINVAL;
		goto error_exit;
	}

	pmp->pm_HugeSectors *= pmp->pm_BlkPerSec;
	pmp->pm_HiddenSects *= pmp->pm_BlkPerSec; /* XXX not used? */
	pmp->pm_FATsecs     *= pmp->pm_BlkPerSec;
	SecPerClust         *= pmp->pm_BlkPerSec;

	pmp->pm_fatblk = pmp->pm_ResSectors * pmp->pm_BlkPerSec;

	if (FAT32(pmp)) {
		pmp->pm_rootdirblk = getulong(b710->bpbRootClust);
		pmp->pm_firstcluster = pmp->pm_fatblk
			+ (pmp->pm_FATs * pmp->pm_FATsecs);
		pmp->pm_fsinfo = getushort(b710->bpbFSInfo) * pmp->pm_BlkPerSec;
	} else {
		pmp->pm_rootdirblk = pmp->pm_fatblk +
			(pmp->pm_FATs * pmp->pm_FATsecs);
		pmp->pm_rootdirsize = (pmp->pm_RootDirEnts * sizeof(struct direntry)
				       + DEV_BSIZE - 1)
			/ DEV_BSIZE; /* in blocks */
		pmp->pm_firstcluster = pmp->pm_rootdirblk + pmp->pm_rootdirsize;
	}

	pmp->pm_maxcluster = (pmp->pm_HugeSectors - pmp->pm_firstcluster) /
	    SecPerClust + 1;
	pmp->pm_fatsize = pmp->pm_FATsecs * DEV_BSIZE; /* XXX not used? */

	if (pmp->pm_fatmask == 0) {
		if (pmp->pm_maxcluster
		    <= ((CLUST_RSRVD - CLUST_FIRST) & FAT12_MASK)) {
			/*
			 * This will usually be a floppy disk. This size makes
			 * sure that one fat entry will not be split across
			 * multiple blocks.
			 */
			pmp->pm_fatmask = FAT12_MASK;
			pmp->pm_fatmult = 3;
			pmp->pm_fatdiv = 2;
		} else {
			pmp->pm_fatmask = FAT16_MASK;
			pmp->pm_fatmult = 2;
			pmp->pm_fatdiv = 1;
		}
	}

	clusters = (pmp->pm_fatsize / pmp->pm_fatmult) * pmp->pm_fatdiv;
	if (pmp->pm_maxcluster >= clusters) {
		printf("Warning: number of clusters (%ld) exceeds FAT "
		    "capacity (%ld)\n", pmp->pm_maxcluster + 1, clusters);
		pmp->pm_maxcluster = clusters - 1;
	}

	if (FAT12(pmp))
		pmp->pm_fatblocksize = 3 * 512;
	else
		pmp->pm_fatblocksize = PAGE_SIZE;
	pmp->pm_fatblocksize = roundup(pmp->pm_fatblocksize,
	    pmp->pm_BytesPerSec);
	pmp->pm_fatblocksec = pmp->pm_fatblocksize / DEV_BSIZE;
	pmp->pm_bnshift = ffs(DEV_BSIZE) - 1;

	/*
	 * Compute mask and shift value for isolating cluster relative byte
	 * offsets and cluster numbers from a file offset.
	 */
	pmp->pm_bpcluster = SecPerClust * DEV_BSIZE;
	pmp->pm_crbomask = pmp->pm_bpcluster - 1;
	pmp->pm_cnshift = ffs(pmp->pm_bpcluster) - 1;

	/*
	 * Check for valid cluster size
	 * must be a power of 2
	 */
	if (pmp->pm_bpcluster ^ (1 << pmp->pm_cnshift)) {
		error = EINVAL;
		goto error_exit;
	}

	/*
	 * Release the bootsector buffer.
	 */
	brelse(bp);
	bp = NULL;

	/*
	 * Check the fsinfo sector if we have one.  Silently fix up our
	 * in-core copy of fp->fsinxtfree if it is unknown (0xffffffff)
	 * or too large.  Ignore fp->fsinfree for now, since we need to
	 * read the entire FAT anyway to fill the inuse map.
	 */
	if (pmp->pm_fsinfo) {
		struct fsinfo *fp;

		if ((error = bread(devvp, pmp->pm_fsinfo, pmp->pm_BytesPerSec,
		    NOCRED, &bp)) != 0)
			goto error_exit;
		fp = (struct fsinfo *)bp->b_data;
		if (!bcmp(fp->fsisig1, "RRaA", 4)
		    && !bcmp(fp->fsisig2, "rrAa", 4)
		    && !bcmp(fp->fsisig3, "\0\0\125\252", 4)) {
			pmp->pm_nxtfree = getulong(fp->fsinxtfree);
			if (pmp->pm_nxtfree > pmp->pm_maxcluster)
				pmp->pm_nxtfree = CLUST_FIRST;
		} else
			pmp->pm_fsinfo = 0;
		brelse(bp);
		bp = NULL;
	}

	/*
	 * Finish initializing pmp->pm_nxtfree (just in case the first few
	 * sectors aren't properly reserved in the FAT).  This completes
	 * the fixup for fp->fsinxtfree, and fixes up the zero-initialized
	 * value if there is no fsinfo.  We will use pmp->pm_nxtfree
	 * internally even if there is no fsinfo.
	 */
	if (pmp->pm_nxtfree < CLUST_FIRST)
		pmp->pm_nxtfree = CLUST_FIRST;

	/*
	 * Allocate memory for the bitmap of allocated clusters, and then
	 * fill it in.
	 */
	pmp->pm_inusemap = malloc(howmany(pmp->pm_maxcluster + 1, N_INUSEBITS)
				  * sizeof(*pmp->pm_inusemap),
				  M_MSDOSFSFAT, M_WAITOK);

	/*
	 * fillinusemap() needs pm_devvp.
	 */
	pmp->pm_devvp = devvp;

	/*
	 * Have the inuse map filled in.
	 */
	if ((error = fillinusemap(pmp)) != 0)
		goto error_exit;

	/*
	 * If they want fat updates to be synchronous then let them suffer
	 * the performance degradation in exchange for the on disk copy of
	 * the fat being correct just about all the time.  I suppose this
	 * would be a good thing to turn on if the kernel is still flakey.
	 */
	if (mp->mnt_flag & MNT_SYNCHRONOUS)
		pmp->pm_flags |= MSDOSFSMNT_WAITONFAT;

	/*
	 * Finish up.
	 */
	if (ronly)
		pmp->pm_flags |= MSDOSFSMNT_RONLY;
	else {
		/* Mark the volume dirty while it is mounted read/write. */
		if ((error = markvoldirty(pmp, 1)) != 0)
			goto error_exit;
		pmp->pm_fmod = 1;
	}
	mp->mnt_data = (qaddr_t) pmp;
	mp->mnt_stat.f_fsid.val[0] = dev2udev(dev);
	mp->mnt_stat.f_fsid.val[1] = mp->mnt_vfc->vfc_typenum;
	MNT_ILOCK(mp);
	mp->mnt_flag |= MNT_LOCAL;
	MNT_IUNLOCK(mp);

	if (pmp->pm_flags & MSDOSFS_LARGEFS)
		msdosfs_fileno_init(mp);

	return 0;

error_exit:
	if (bp)
		brelse(bp);
	if (cp != NULL) {
		DROP_GIANT();
		g_topology_lock();
		g_vfs_close(cp, td);
		g_topology_unlock();
		PICKUP_GIANT();
	}
	if (pmp) {
		if (pmp->pm_inusemap)
			free(pmp->pm_inusemap, M_MSDOSFSFAT);
		free(pmp, M_MSDOSFSMNT);
		mp->mnt_data = (qaddr_t)0;
	}
	return (error);
}

/*
 * Unmount the filesystem described by mp.
 */
static int
msdosfs_unmount(struct mount *mp, int mntflags, struct thread *td)
{
	struct msdosfsmount *pmp;
	int error, flags;

	flags = 0;
	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;
	error = vflush(mp, 0, flags, td);
	if (error)
		return error;
	pmp = VFSTOMSDOSFS(mp);
	if (pmp->pm_flags & MSDOSFSMNT_KICONV && msdosfs_iconv) {
		if (pmp->pm_w2u)
			msdosfs_iconv->close(pmp->pm_w2u);
		if (pmp->pm_u2w)
			msdosfs_iconv->close(pmp->pm_u2w);
		if (pmp->pm_d2u)
			msdosfs_iconv->close(pmp->pm_d2u);
		if (pmp->pm_u2d)
			msdosfs_iconv->close(pmp->pm_u2d);
	}

	/* If the volume was mounted read/write, mark it clean now. */
	if ((pmp->pm_flags & MSDOSFSMNT_RONLY) == 0) {
		error = markvoldirty(pmp, 0);
		if (error && (flags & FORCECLOSE) == 0)
			return (error);
	}
#ifdef MSDOSFS_DEBUG
	{
		struct vnode *vp = pmp->pm_devvp;

		VI_LOCK(vp);
		vn_printf(vp,
		    "msdosfs_umount(): just before calling VOP_CLOSE()\n");
		printf("freef %p, freeb %p, mount %p\n",
		    TAILQ_NEXT(vp, v_freelist), vp->v_freelist.tqe_prev,
		    vp->v_mount);
		printf("cleanblkhd %p, dirtyblkhd %p, numoutput %ld, type %d\n",
		    TAILQ_FIRST(&vp->v_bufobj.bo_clean.bv_hd),
		    TAILQ_FIRST(&vp->v_bufobj.bo_dirty.bv_hd),
		    vp->v_bufobj.bo_numoutput, vp->v_type);
		VI_UNLOCK(vp);
	}
#endif
	DROP_GIANT();
	g_topology_lock();
	g_vfs_close(pmp->pm_cp, td);
	g_topology_unlock();
	PICKUP_GIANT();
	vrele(pmp->pm_devvp);
	free(pmp->pm_inusemap, M_MSDOSFSFAT);
	if (pmp->pm_flags & MSDOSFS_LARGEFS) {
		msdosfs_fileno_free(mp);
	}
	free(pmp, M_MSDOSFSMNT);
	mp->mnt_data = (qaddr_t)0;
	MNT_ILOCK(mp);
	mp->mnt_flag &= ~MNT_LOCAL;
	MNT_IUNLOCK(mp);
	return (error);
}

static int
msdosfs_root(struct mount *mp, int flags, struct vnode **vpp, struct thread *td)
{
	struct msdosfsmount *pmp = VFSTOMSDOSFS(mp);
	struct denode *ndep;
	int error;

#ifdef MSDOSFS_DEBUG
	printf("msdosfs_root(); mp %p, pmp %p\n", mp, pmp);
#endif
	error = deget(pmp, MSDOSFSROOT, MSDOSFSROOT_OFS, &ndep);
	if (error)
		return (error);
	*vpp = DETOV(ndep);
	return (0);
}

static int
msdosfs_statfs(struct mount *mp, struct statfs *sbp, struct thread *td)
{
	struct msdosfsmount *pmp;

	pmp = VFSTOMSDOSFS(mp);
	sbp->f_bsize = pmp->pm_bpcluster;
	sbp->f_iosize = pmp->pm_bpcluster;
	sbp->f_blocks = pmp->pm_maxcluster + 1;
	sbp->f_bfree = pmp->pm_freeclustercount;
	sbp->f_bavail = pmp->pm_freeclustercount;
	sbp->f_files = pmp->pm_RootDirEnts;			/* XXX */
	sbp->f_ffree = 0;	/* what to put in here? */
	return (0);
}

static int
msdosfs_sync(struct mount *mp, int waitfor, struct thread *td)
{
	struct vnode *vp, *nvp;
	struct denode *dep;
	struct msdosfsmount *pmp = VFSTOMSDOSFS(mp);
	int error, allerror = 0;

	/*
	 * If we ever switch to not updating all of the fats all the time,
	 * this would be the place to update them from the first one.
	 */
	if (pmp->pm_fmod != 0) {
		if (pmp->pm_flags & MSDOSFSMNT_RONLY)
			panic("msdosfs_sync: rofs mod");
		else {
			/* update fats here */
		}
	}
	/*
	 * Write back each (modified) denode.
	 */
	MNT_ILOCK(mp);
loop:
	MNT_VNODE_FOREACH(vp, mp, nvp) {
		VI_LOCK(vp);
		if (vp->v_type == VNON || (vp->v_iflag & VI_DOOMED)) {
			VI_UNLOCK(vp);
			continue;
		}
		MNT_IUNLOCK(mp);
		dep = VTODE(vp);
		if ((dep->de_flag &
		    (DE_ACCESS | DE_CREATE | DE_UPDATE | DE_MODIFIED)) == 0 &&
		    (vp->v_bufobj.bo_dirty.bv_cnt == 0 ||
		    waitfor == MNT_LAZY)) {
			VI_UNLOCK(vp);
			MNT_ILOCK(mp);
			continue;
		}
		error = vget(vp, LK_EXCLUSIVE | LK_NOWAIT | LK_INTERLOCK, td);
		if (error) {
			MNT_ILOCK(mp);
			if (error == ENOENT)
				goto loop;
			continue;
		}
		error = VOP_FSYNC(vp, waitfor, td);
		if (error)
			allerror = error;
		VOP_UNLOCK(vp, 0, td);
		vrele(vp);
		MNT_ILOCK(mp);
	}
	MNT_IUNLOCK(mp);

	/*
	 * Flush filesystem control info.
	 */
	if (waitfor != MNT_LAZY) {
		vn_lock(pmp->pm_devvp, LK_EXCLUSIVE | LK_RETRY, td);
		error = VOP_FSYNC(pmp->pm_devvp, waitfor, td);
		if (error)
			allerror = error;
		VOP_UNLOCK(pmp->pm_devvp, 0, td);
	}
	return (allerror);
}

static int
msdosfs_fhtovp(struct mount *mp, struct fid *fhp, struct vnode **vpp)
{
	struct msdosfsmount *pmp = VFSTOMSDOSFS(mp);
	struct defid *defhp = (struct defid *) fhp;
	struct denode *dep;
	int error;

	error = deget(pmp, defhp->defid_dirclust, defhp->defid_dirofs, &dep);
	if (error) {
		*vpp = NULLVP;
		return (error);
	}
	*vpp = DETOV(dep);
	vnode_create_vobject(*vpp, dep->de_FileSize, curthread);
	return (0);
}

static struct vfsops msdosfs_vfsops = {
	.vfs_fhtovp =		msdosfs_fhtovp,
	.vfs_mount =		msdosfs_mount,
	.vfs_cmount =		msdosfs_cmount,
	.vfs_root =		msdosfs_root,
	.vfs_statfs =		msdosfs_statfs,
	.vfs_sync =		msdosfs_sync,
	.vfs_unmount =		msdosfs_unmount,
};

VFS_SET(msdosfs_vfsops, msdosfs, 0);
MODULE_VERSION(msdosfs, 1);
