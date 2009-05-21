/*
 * mpqfs: FUSE module for libmpq
 * Copyright (C) 2008 Georg Lukas <georg@op-co.de>
 *
 * This program can be distributed under the terms of the GNU GPL.
 *
 * Compilation:
 * $ cc -D_FILE_OFFSET_BITS=64  -lfuse -lmpq  mpqfs.c   -o mpqfs
 *
 * Usage:
 * $ ./mpqfs <archive> [-d] <mountpoint>
 *
 * THIS CODE IS A HACK. ERROR HANDLING TO BE IMPLEMENTED LATER.
 * PROPER DIRECTORY STRUCTURE? FORGET IT FOR NOW!
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

struct mpq_dir {
	char *name;
	unsigned int subdir_c;
	struct mpq_dir *subdirs;
	unsigned int file_c;
	char **files;
} root;

char *implicit[] = {
	"(listfile)",
	"(attributes)",
	"(signatures)",
	NULL
};

uint32_t mpq_parse_lf(mpq_archive_s *a, char *listfile, struct mpq_dir *dir) {
	if (libmpq__archive_files(a, &dir->file_c) != 0) {
		printf("Error getting number of files in archive.\n");
		return 0;
	}
	dir->files = malloc(sizeof(char*) * dir->file_c);
	assert(dir->files != NULL);
	memset(dir->files, 0, sizeof(char*) * dir->file_c);

	char *filename = strtok(listfile, "\r\n");
	uint32_t fn;
	while (filename) {
		if (libmpq__file_number(a, filename, &fn) != 0) {
			printf("missing %s\n", filename);
		} else {
			assert(fn < dir->file_c);
			if (dir->files[fn])
				printf("double %s (%s)\n", filename, dir->files[fn]);
			else
				dir->files[fn] = filename;
		}
		filename = strtok(NULL, "\r\n");
	}
	char **i;
	for (i = implicit; *i; i++)
		if ((libmpq__file_number(a, *i, &fn) == 0) &&
		    (dir->files[fn] == NULL)) {
			dir->files[fn] = *i;
		}
	return dir->file_c;
}

mpq_archive_s *archive;
char *listfile;

static int mpq_getattr(const char *path, struct stat *stbuf)
{
	int res = 0;
	memset(stbuf, 0, sizeof(struct stat));
	if(strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		stbuf->st_size = root.file_c;
	}
	else {
		uint32_t fn = 0;
		off_t fsize = 0;
		if (libmpq__file_number(archive, path+1, &fn) != 0)
			return -ENOENT;
		libmpq__file_unpacked_size(archive, fn, &fsize);

		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = fsize;
	}
	//else
	//   res = -ENOENT;

	return res;
}

static int mpq_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi)
{
    (void) offset;
    (void) fi;

    if(strcmp(path, "/") != 0)
        return -ENOENT;

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

	uint32_t i;
	for (i = 0; i < root.file_c; i++)
		if (filler(buf, root.files[i], NULL, 0) != 0)
			break;

    return 0;
}

static int mpq_open(const char *path, struct fuse_file_info *fi)
{
	uint32_t fn;
	if (libmpq__file_number(archive, path+1, &fn) != 0)
		return -ENOENT;

	if((fi->flags & 3) != O_RDONLY)
		return -EACCES;

	fi->fh = fn;

	return 0;
}

static int mpq_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
	(void) fi;

	uint32_t fn = 0;
	off_t fsize = 0;
	size_t copied = 0;

	if (libmpq__file_number(archive, path+1, &fn) != 0)
		return -ENOENT;
	assert(fi->fh == fn);

	libmpq__file_unpacked_size(archive, fn, &fsize);


	if (offset < fsize) {
		/* do not read over EOF */
		if (offset + size > fsize)
			size = fsize - offset;

		off_t blocksize;

		libmpq__block_open_offset(archive, fn);
		libmpq__block_unpacked_size(archive, fn, 0, &blocksize);

		uint32_t blocknumber = offset / blocksize;

		/* check if the start is in the middle of a block */
		uint32_t prefix = (offset % blocksize);
		if (prefix != 0) {
			/* we have to get the middle of a data block manually */
			uint8_t *data = malloc(blocksize);
			off_t trans, needed;

			libmpq__block_read(archive, fn, blocknumber, data, blocksize, &trans);

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
			libmpq__block_read(archive, fn, blocknumber, (uint8_t*)buf + copied,
					MIN(blocksize, size - copied), &trans);
			copied += trans;
		}
		libmpq__block_close_offset(archive, fn);
	}

	return copied;
}

static struct fuse_operations mpq_oper = {
    .getattr	= mpq_getattr,
    .readdir	= mpq_readdir,
    .open	= mpq_open,
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
	libmpq__file_unpacked_size(archive, listfile_number, &listfile_size);
	listfile = malloc(listfile_size);
	libmpq__file_read(archive, listfile_number, (uint8_t*)listfile, listfile_size, NULL);

	mpq_parse_lf(archive, listfile, &root);

	return fuse_main(argc-1, argv+1, &mpq_oper, NULL);
}
