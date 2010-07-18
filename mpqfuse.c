/*
 * mpqfuse: FUSE module for libmpq
 * Copyright (C) 2009 Georg Lukas <georg@op-co.de>
 *
 * This program can be distributed under the terms of the GNU GPL.
 *
 * Compilation:
 * $ cc -D_FILE_OFFSET_BITS=64  -lfuse -lmpq  mpqfuse.c   -o mpqfuse
 *
 * Usage:
 * $ ./mpqfuse <archive> -s [-d] <mountpoint>
 *
 * THIS CODE IS A HACK. ERROR HANDLING TO BE IMPLEMENTED LATER.
 * WARNING: mpqfuse is not thread-safe! Never forget the -s parameter!
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <libmpq/mpq.h>
#include <stdlib.h>
#include <assert.h>

#define FUSE_USE_VERSION 26
#include <fuse.h>

#define MIN(a, b) (((a) < (b))? (a) : (b))

/* libmpq call macro to not fail silently */
#define CHECK(call) do {								\
		int __ret = call;							\
		if (__ret < 0) {							\
			fprintf(stderr, "MPQFUSE: ERROR CALLING LIBMPQ (%s:%d):\n"	\
					"%s = %d\n\t(%s)\n",				\
					__FILE__, __LINE__,				\
					#call, __ret, libmpq__strerror(__ret));		\
			abort();							\
		}									\
	} while (0);

#ifdef DEBUG
#define DPRINTF(fmt, param...) fprintf(stderr, "*** %s: " fmt, __FUNCTION__, ##param)
#else
#define DPRINTF(...)
#endif

struct mpq_file {
	char *name;
	off_t size;
	unsigned int fn;
};

struct mpq_dir {
	char *name;
	unsigned int subdir_c;
	struct mpq_dir *subdirs;
	unsigned int file_c;
	struct mpq_file *files;
} root;

char *implicit[] = {
	"(listfile)",
	"(attributes)",
	"(signature)",
	"(signatures)",
	NULL
};

void init_mpq_dir(struct mpq_dir *dir, char *name) {
	dir->name = name;
	dir->subdir_c = 0;
	dir->subdirs = 0;
	dir->file_c = 0;
	dir->files = 0;
}

struct mpq_dir *open_subdir(struct mpq_dir *dir, const char *name, int createnew) {
	int idx;
	for (idx = 0; idx < dir->subdir_c; idx++) {
		if (strcasecmp(dir->subdirs[idx].name, name) == 0) {
			return &dir->subdirs[idx];
		}
	}
	if (createnew) {
		dir->subdir_c++;
		dir->subdirs = realloc(dir->subdirs, dir->subdir_c * sizeof(struct mpq_dir));
		init_mpq_dir(&dir->subdirs[idx], strdup(name));
		return &dir->subdirs[idx];
	}
	return NULL;
};

struct mpq_file *open_file(struct mpq_dir *dir, const char *name) {
	int idx;
	for (idx = 0; idx < dir->file_c; idx++) {
		if (strcasecmp(dir->files[idx].name, name) == 0) {
			return &dir->files[idx];
		}
	}
	return NULL;
};

struct mpq_dir *open_file_dir(struct mpq_dir *root, char **filename, int createnew) {
	char *next;
	struct mpq_dir *dir = root;
	char dirbuf[2048];
	while ((next = strpbrk(*filename, "/\\")) != NULL) {
		snprintf(dirbuf, 2048, "%.*s", next - *filename, *filename);
		dir = open_subdir(dir, dirbuf, createnew);
		//DPRINTF("subdir %s: %p\n", dirbuf, dir);
		if (dir == NULL)
			return NULL;
		*filename = next+1;
	}
	return dir;
}

void add_file(struct mpq_dir *root, char *filename, off_t size, unsigned int fn) {
	struct mpq_dir *dir = open_file_dir(root, &filename, 1);
	//DPRINTF("add_file %s\n", filename);
	int idx = dir->file_c++;
	dir->files = realloc(dir->files, sizeof(struct mpq_file) * dir->file_c);
	dir->files[idx].name = strdup(filename);
	dir->files[idx].size = size;
	dir->files[idx].fn = fn;
}

uint32_t mpq_parse_lf(mpq_archive_s *a, char *listfile, struct mpq_dir *dir) {
	unsigned int files;

	init_mpq_dir(dir, NULL);

	if (libmpq__archive_files(a, &files) != 0) {
		fprintf(stderr, "Error getting number of files in archive.\n");
		return 0;
	}
	char *lf_ptr;
	char *filename = strtok_r(listfile, "\r\n", &lf_ptr);
	uint32_t fn;
	while (filename) {
		if (libmpq__file_number(a, filename, &fn) != 0) {
			fprintf(stderr, "orphan %s\n", filename);
		} else {
			add_file(&root, filename, 0, fn);
		}
		filename = strtok_r(NULL, "\r\n", &lf_ptr);
	}
	char **i;
	for (i = implicit; *i; i++)
		if (libmpq__file_number(a, *i, &fn) == 0)
			add_file(&root, *i, 0, fn);
	return files;
}

mpq_archive_s *archive;
char *listfile;

static int mpq_getattr(const char *path, struct stat *stbuf)
{
	DPRINTF("%s %p\n", path, stbuf);
	memset(stbuf, 0, sizeof(struct stat));

	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		stbuf->st_size = root.subdir_c + root.file_c;
		return 0;
	}

	char *fn = path + 1;
	struct mpq_dir *dir, *subdir;
	struct mpq_file *file;
	dir = open_file_dir(&root, &fn, 0);
	if (!dir)
		return -ENOENT;

	subdir = open_subdir(dir, fn, 0);
	if (subdir) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		stbuf->st_size = subdir->subdir_c + subdir->file_c;
		return 0;
	}

	file = open_file(dir, fn);
	if (file) {
		off_t fsize = 0;
		CHECK(libmpq__file_unpacked_size(archive, file->fn, &fsize));
		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = fsize;
		return 0;
	}

	return -ENOENT;
}

static int mpq_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi)
{
	(void) offset;
	(void) fi;

	int pbuflen = strlen(path) + 1;
	char pbuf[pbuflen];
	char *fn = pbuf;
	strcpy(pbuf, path + 1);
	if (strlen(pbuf) > 0)
		strcat(pbuf, "/");

	DPRINTF("%s\n", fn);
	struct mpq_dir *dir = open_file_dir(&root, &fn, 0);
	if (!dir)
		return -ENOENT;
	DPRINTF("%s -> %s\n", path, dir->name);

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	uint32_t i;
	for (i = 0; i < dir->subdir_c; i++) {
		if (filler(buf, dir->subdirs[i].name, NULL, 0) != 0)
			break;
	}
	for (i = 0; i < dir->file_c; i++) {
		if (filler(buf, dir->files[i].name, NULL, 0) != 0)
			break;
	}

	return 0;
}

static int mpq_open(const char *path, struct fuse_file_info *fi)
{
	char *fn = path + 1;
	struct mpq_dir *dir;
	struct mpq_file *file;
	dir = open_file_dir(&root, &fn, 0);
	if (!dir)
		return -ENOENT;

	if ((file = open_file(dir, fn)) == 0)
		return -ENOENT;

	if((fi->flags & 3) != O_RDONLY)
		return -EACCES;

	fi->fh = file->fn;
	CHECK(libmpq__block_open_offset(archive, fi->fh));

	return 0;
}

static int mpq_close(const char *path, struct fuse_file_info *fi)
{
	CHECK(libmpq__block_close_offset(archive, fi->fh));
	return 0;
}


static int mpq_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
	(void) fi;

	uint32_t fn = fi->fh;
	off_t fsize = 0;
	size_t copied = 0;

	CHECK(libmpq__file_unpacked_size(archive, fn, &fsize));


	if (offset < fsize) {
		/* do not read over EOF */
		if (offset + size > fsize)
			size = fsize - offset;

		off_t blocksize;

		CHECK(libmpq__block_unpacked_size(archive, fn, 0, &blocksize));

		uint32_t blocknumber = offset / blocksize;

		/* check if the start is in the middle of a block */
		uint32_t prefix = (offset % blocksize);

		DPRINTF("'%s': block=%d prefix=%d size=%zd bs=%zd\n",
				path, blocknumber, prefix, size, blocksize);
		if (prefix != 0) {
			/* we have to get the middle of a data block manually */
			uint8_t *data = malloc(blocksize);
			off_t trans, needed;

			CHECK(libmpq__block_read(archive, fn, blocknumber, data, blocksize, &trans));

			needed = trans - prefix;
			if (needed > size)
				needed = size;
			memcpy(buf, data + prefix, needed);
			free(data);
			copied += needed;
		}
		while (copied < size) {
			off_t trans;
			blocknumber = (offset + copied) / blocksize;
			CHECK(libmpq__block_read(archive, fn, blocknumber, (uint8_t*)buf + copied,
					MIN(blocksize, size - copied), &trans));
			copied += trans;
		}
	}

	return copied;
}

static struct fuse_operations mpq_oper = {
    .getattr	= mpq_getattr,
    .readdir	= mpq_readdir,
    .open	= mpq_open,
    .release	= mpq_close,
    .read	= mpq_read,
};

int main(int argc, char *argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage\n%s <archive.mpq> [-d] <mountpoint>\n",
				argv[0]);
		return 1;
	}
	if (libmpq__archive_open(&archive, argv[1], -1) != 0) {
		perror("libmpq__archive_open()");
		return 1;
	}

	uint32_t listfile_number;
	off_t listfile_size;
	if (libmpq__file_number(archive, "(listfile)", &listfile_number) != 0) {
		fprintf(stderr, "No listfile in '%s'.\n", argv[1]);
		libmpq__archive_close(archive);
		return 1;
	}
	CHECK(libmpq__file_unpacked_size(archive, listfile_number, &listfile_size));
	listfile = malloc(listfile_size + 1);
	CHECK(libmpq__file_read(archive, listfile_number, (uint8_t*)listfile, listfile_size, NULL));
	listfile[listfile_size] = '\0';

	mpq_parse_lf(archive, listfile, &root);

#if 0
	int dbg;
	for (dbg = 0; dbg < root.file_c; dbg++) {
		DPRINTF(">%s\n", root.files[dbg].name);
	}
#endif
	return fuse_main(argc-1, argv+1, &mpq_oper, NULL);
}
