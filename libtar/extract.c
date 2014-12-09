/*
**  Copyright 1998-2003 University of Illinois Board of Trustees
**  Copyright 1998-2003 Mark D. Roth
**  All rights reserved.
**
**  extract.c - libtar code to extract a file from a tar archive
**
**  Mark D. Roth <roth@uiuc.edu>
**  Campus Information Technologies and Educational Services
**  University of Illinois at Urbana-Champaign
*/

#include <internal.h>

#include <stdio.h>
#include <sys/param.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <utime.h>

#ifdef STDC_HEADERS
# include <stdlib.h>
#endif

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif


static int
tar_set_file_perms(TAR *t, char *realname)
{
	mode_t mode;
	uid_t uid;
	gid_t gid;
	struct utimbuf ut;
	char *filename;

	filename = (realname ? realname : th_get_pathname(t));
	mode = th_get_mode(t);
	uid = th_get_uid(t);
	gid = th_get_gid(t);
	ut.modtime = ut.actime = th_get_mtime(t);

#ifdef TAR_DEBUG_VERBOSE
	printf("   ==> setting perms: %s (mode %04o, uid %d, gid %d)\n",
	       filename, mode, uid, gid);
#endif

	/* change owner/group */
	if (geteuid() == 0)
#ifdef HAVE_LCHOWN
		if (lchown(filename, uid, gid) == -1)
		{
	#ifdef TAR_DEBUG_VERBOSE
			fprintf(stderr, "lchown(\"%s\", %d, %d): %s\n",
				filename, uid, gid, strerror(errno));
	#endif
#else /* ! HAVE_LCHOWN */
		if (!TH_ISSYM(t) && chown(filename, uid, gid) == -1)
		{
	#ifdef TAR_DEBUG_VERBOSE
			fprintf(stderr, "chown(\"%s\", %d, %d): %s\n",
				filename, uid, gid, strerror(errno));
	#endif
#endif /* HAVE_LCHOWN */
			return -1;
		}

	/* change access/modification time */
	if (!TH_ISSYM(t) && utime(filename, &ut) == -1)
	{
#ifdef TAR_DEBUG_VERBOSE
		perror("utime()");
#endif
		return -1;
	}

	/* change permissions */
	if (!TH_ISSYM(t) && chmod(filename, mode) == -1)
	{
#ifdef TAR_DEBUG_VERBOSE
		perror("chmod()");
#endif
		return -1;
	}

	return 0;
}


/* switchboard */
int
tar_extract_file(TAR *t, char *realname, char *prefix)
{
	int i;
	char *lnp;
	int pathname_len;
	int realname_len;

	if (t->options & TAR_NOOVERWRITE)
	{
		struct stat s;

		if (lstat(realname, &s) == 0 || errno != ENOENT)
		{
			errno = EEXIST;
			return -1;
		}
	}

	if (TH_ISDIR(t))
	{
#ifndef TAR_DEBUG_SUPPRESS
		printf("dir");
#endif
#ifdef TAR_DEBUG_VERBOSE
		printf("\n");
#endif
		i = tar_extract_dir(t, realname);
		if (i == 1)
			i = 0;
	}
	else if (TH_ISLNK(t)) {
#ifndef TAR_DEBUG_SUPPRESS
		printf("link");
#endif
#ifdef TAR_DEBUG_VERBOSE
		printf("\n");
#endif
		i = tar_extract_hardlink(t, realname, prefix);
	}
	else if (TH_ISSYM(t)) {
#ifndef TAR_DEBUG_SUPPRESS
		printf("sym");
#endif
#ifdef TAR_DEBUG_VERBOSE
		printf("\n");
#endif
		i = tar_extract_symlink(t, realname);
	}
	else if (TH_ISCHR(t)) {
#ifndef TAR_DEBUG_SUPPRESS
		printf("chr");
#endif
#ifdef TAR_DEBUG_VERBOSE
		printf("\n");
#endif
		i = tar_extract_chardev(t, realname);
	}
	else if (TH_ISBLK(t)) {
#ifndef TAR_DEBUG_SUPPRESS
		printf("blk");
#endif
#ifdef TAR_DEBUG_VERBOSE
		printf("\n");
#endif
		i = tar_extract_blockdev(t, realname);
	}
	else if (TH_ISFIFO(t)) {
#ifndef TAR_DEBUG_SUPPRESS
		printf("fifo");
#endif
#ifdef TAR_DEBUG_VERBOSE
		printf("\n");
#endif
		i = tar_extract_fifo(t, realname);
	}
	else /* if (TH_ISREG(t)) */ {
#ifndef TAR_DEBUG_SUPPRESS
		printf("reg");
#endif
#ifdef TAR_DEBUG_VERBOSE
		printf("\n");
#endif
		i = tar_extract_regfile(t, realname);
	}

	if (i != 0) {
#ifdef TAR_DEBUG_VERBOSE
		printf("\nFAILED RESTORE OF FILE i: %s\n", realname);
#endif
		return i;
	}

	i = tar_set_file_perms(t, realname);
	if (i != 0) {
#ifdef TAR_DEBUG_VERBOSE
		printf("\nFAILED SETTING PERMS: %d\n", i);
#endif
		return i;
	}

#ifdef HAVE_SELINUX
	if((t->options & TAR_STORE_SELINUX) && t->th_buf.selinux_context != NULL) {
	#ifdef TAR_DEBUG_VERBOSE
		printf(" Restoring SELinux context %s to file %s\n", t->th_buf.selinux_context, realname);
	#endif
		if(lsetfilecon(realname, t->th_buf.selinux_context) < 0)
			fprintf(stderr, "Failed to restore SELinux context %s!\n", strerror(errno));
	}
#endif
/*
	pathname_len = strlen(th_get_pathname(t)) + 1;
	realname_len = strlen(realname) + 1;
	lnp = (char *)calloc(1, pathname_len + realname_len);
	if (lnp == NULL)
		return -1;
	strcpy(&lnp[0], th_get_pathname(t));
	strcpy(&lnp[pathname_len], realname);
#ifdef TAR_DEBUG_VERBOSE
	printf("tar_extract_file(): calling libtar_hash_add(): key=\"%s\", "
	       "value=\"%s\"\n", th_get_pathname(t), realname);
#endif
	if (libtar_hash_add(t->h, lnp) != 0)
		return -1;
	free(lnp);
*/
	return 0;
}


/* extract regular file */
int
tar_extract_regfile(TAR *t, char *realname)
{
	//mode_t mode;
	size_t size;
	//uid_t uid;
	//gid_t gid;
	int fdout;
	int i, k;
	char buf[T_BLOCKSIZE];
	char *filename;

	fflush(NULL);
#ifdef TAR_DEBUG_VERBOSE
	printf("==> tar_extract_regfile(t=0x%lx, realname=\"%s\")\n", t,
	       realname);
#endif

	if (!TH_ISREG(t))
	{
		errno = EINVAL;
		return -1;
	}

	filename = (realname ? realname : th_get_pathname(t));
	//mode = th_get_mode(t);
	size = th_get_size(t);
	//uid = th_get_uid(t);
	//gid = th_get_gid(t);

	if (mkdirhier(dirname(filename)) == -1)
		return -1;

#ifdef TAR_DEBUG_VERBOSE
	//printf("  ==> extracting: %s (mode %04o, uid %d, gid %d, %d bytes)\n",
	//       filename, mode, uid, gid, size);
	printf("  ==> extracting: %s (file size %d bytes)\n",
	       filename, size);
#endif
	fdout = open(filename, O_WRONLY | O_CREAT | O_TRUNC
#ifdef O_BINARY
		     | O_BINARY
#endif
		    , 0666);
	if (fdout == -1)
	{
#ifdef TAR_DEBUG_VERBOSE
		perror("open()");
#endif
		return -1;
	}

#if 0
	/* change the owner.  (will only work if run as root) */
	if (fchown(fdout, uid, gid) == -1 && errno != EPERM)
	{
#ifdef TAR_DEBUG_VERBOSE
		perror("fchown()");
#endif
		return -1;
	}

	/* make sure the mode isn't inheritted from a file we're overwriting */
	if (fchmod(fdout, mode & 07777) == -1)
	{
#ifdef TAR_DEBUG_VERBOSE
		perror("fchmod()");
#endif
		return -1;
	}
#endif

	/* extract the file */
	for (i = size; i > 0; i -= T_BLOCKSIZE)
	{
		k = tar_block_read(t, buf);
		if (k != T_BLOCKSIZE)
		{
			if (k != -1)
				errno = EINVAL;
			return -1;
		}

		/* write block to output file */
		if (write(fdout, buf,
			  ((i > T_BLOCKSIZE) ? T_BLOCKSIZE : i)) == -1)
			return -1;
	}

	/* close output file */
	if (close(fdout) == -1)
		return -1;

#ifdef TAR_DEBUG_VERBOSE
	printf("### done extracting %s\n", filename);
#endif

	return 0;
}


/* skip regfile */
int
tar_skip_regfile(TAR *t)
{
	int i, k;
	size_t size;
	char buf[T_BLOCKSIZE];

	if (!TH_ISREG(t))
	{
		errno = EINVAL;
		return -1;
	}

	size = th_get_size(t);
	for (i = size; i > 0; i -= T_BLOCKSIZE)
	{
		k = tar_block_read(t, buf);
		if (k != T_BLOCKSIZE)
		{
			if (k != -1)
				errno = EINVAL;
			return -1;
		}
	}

	return 0;
}


/* hardlink */
int
tar_extract_hardlink(TAR * t, char *realname, char *prefix)
{
	char *filename;
	char *linktgt = NULL;
	char *lnp;
	libtar_hashptr_t hp;

	if (!TH_ISLNK(t))
	{
		errno = EINVAL;
		return -1;
	}

	filename = (realname ? realname : th_get_pathname(t));
	if (mkdirhier(dirname(filename)) == -1)
		return -1;
	libtar_hashptr_reset(&hp);
	if (libtar_hash_getkey(t->h, &hp, th_get_linkname(t),
			       (libtar_matchfunc_t)libtar_str_match) != 0)
	{
		lnp = (char *)libtar_hashptr_data(&hp);
		linktgt = &lnp[strlen(lnp) + 1];
	}
	else
		linktgt = th_get_linkname(t);
	char *newtgt = strdup(linktgt);
	sprintf(linktgt, "%s/%s", prefix, newtgt);
#ifdef TAR_DEBUG_VERBOSE
	printf("  ==> extracting: %s (link to %s)\n", filename, linktgt);
#endif
	if (link(linktgt, filename) == -1)
	{
#ifdef TAR_DEBUG_VERBOSE
		perror("link()");
		printf("Failed restore of hardlink '%s' but returning as if nothing bad happened anyway\n", filename);
#endif
		return 0; // Used to be -1
	}

	return 0;
}


/* symlink */
int
tar_extract_symlink(TAR *t, char *realname)
{
	char *filename;

	if (!TH_ISSYM(t))
	{
#ifdef TAR_DEBUG_VERBOSE
		printf("not a sym\n");
#endif
		errno = EINVAL;
		return -1;
	}

	filename = (realname ? realname : th_get_pathname(t));
#ifdef TAR_DEBUG_VERBOSE
	printf("file: %s\n", filename);
#endif
	if (mkdirhier(dirname(filename)) == -1) {
		printf("mkdirhier\n");
		return -1;
	}

	if (unlink(filename) == -1 && errno != ENOENT) {
#ifdef TAR_DEBUG_VERBOSE
		printf("unlink\n");
#endif
		return -1;
	}

#ifdef TAR_DEBUG_VERBOSE
	printf("  ==> extracting: %s (symlink to %s)\n",
	       filename, th_get_linkname(t));
#endif
	if (symlink(th_get_linkname(t), filename) == -1)
	{
#ifdef TAR_DEBUG_VERBOSE
		perror("symlink()");
#endif
		return -1;
	}

	return 0;
}


/* character device */
int
tar_extract_chardev(TAR *t, char *realname)
{
	mode_t mode;
	unsigned long devmaj, devmin;
	char *filename;

	if (!TH_ISCHR(t))
	{
		errno = EINVAL;
		return -1;
	}

	filename = (realname ? realname : th_get_pathname(t));
	mode = th_get_mode(t);
	devmaj = th_get_devmajor(t);
	devmin = th_get_devminor(t);

	if (mkdirhier(dirname(filename)) == -1)
		return -1;

#ifdef TAR_DEBUG_VERBOSE
	printf("  ==> extracting: %s (character device %ld,%ld)\n",
	       filename, devmaj, devmin);
#endif
	if (mknod(filename, mode | S_IFCHR,
		  compat_makedev(devmaj, devmin)) == -1)
	{
#ifdef TAR_DEBUG_VERBOSE
		printf("mknod() failed, returning good anyway");
#endif
		return 0;
	}

	return 0;
}


/* block device */
int
tar_extract_blockdev(TAR *t, char *realname)
{
	mode_t mode;
	unsigned long devmaj, devmin;
	char *filename;

	if (!TH_ISBLK(t))
	{
		errno = EINVAL;
		return -1;
	}

	filename = (realname ? realname : th_get_pathname(t));
	mode = th_get_mode(t);
	devmaj = th_get_devmajor(t);
	devmin = th_get_devminor(t);

	if (mkdirhier(dirname(filename)) == -1)
		return -1;

#ifdef TAR_DEBUG_VERBOSE
	printf("  ==> extracting: %s (block device %ld,%ld)\n",
	       filename, devmaj, devmin);
#endif
	if (mknod(filename, mode | S_IFBLK,
		  compat_makedev(devmaj, devmin)) == -1)
	{
#ifdef TAR_DEBUG_VERBOSE
		printf("mknod() failed but returning anyway");
#endif
		return 0;
	}

	return 0;
}


/* directory */
int
tar_extract_dir(TAR *t, char *realname)
{
	mode_t mode;
	char *filename;
	if (!TH_ISDIR(t))
	{
		errno = EINVAL;
		return -1;
	}

	filename = (realname ? realname : th_get_pathname(t));
	mode = th_get_mode(t);

	if (mkdirhier(dirname(filename)) == -1) {
#ifdef TAR_DEBUG_VERBOSE
		printf("tar_extract_dir mkdirhier failed\n");
#endif
		return -1;
	}

#ifdef TAR_DEBUG_VERBOSE
	printf("  ==> extracting: %s (mode %04o, directory)\n", filename,
	       mode);
#endif
	if (mkdir(filename, mode) == -1)
	{
		if (errno == EEXIST)
		{
#ifdef TAR_DEBUG_VERBOSE
			printf("  *** using existing directory");
#endif
		}
		else
		{
#ifdef TAR_DEBUG_VERBOSE
			perror("mkdir()");
#endif
			return -1;
		}
	}

	return 0;
}


/* FIFO */
int
tar_extract_fifo(TAR *t, char *realname)
{
	mode_t mode;
	char *filename;

	if (!TH_ISFIFO(t))
	{
		errno = EINVAL;
		return -1;
	}

	filename = (realname ? realname : th_get_pathname(t));
	mode = th_get_mode(t);

	if (mkdirhier(dirname(filename)) == -1)
		return -1;

#ifdef TAR_DEBUG_VERBOSE
	printf("  ==> extracting: %s (fifo)\n", filename);
#endif
	if (mkfifo(filename, mode) == -1)
	{
#ifdef TAR_DEBUG_VERBOSE
		perror("mkfifo()");
#endif
		return -1;
	}

	return 0;
}


