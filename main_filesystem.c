#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>

#include <sys/stat.h>

#include "shfs.h"


void fs_remote_read(struct shfs *fs, struct shfs_file_op *op)
{
	struct shfs_message m_alloc = {0};
	struct shfs_message *m = &m_alloc;
	char tmp_fname[1024];
	FILE* fd = NULL;
	size_t len;

	sprintf(tmp_fname, "%s/%s", fs->tmpdir, op->name);

	fprintf(stderr, "FS REMOTE read %d/%d offset '%s' [%s]\n",
		(int)op->offset,
		(int)file_size(tmp_fname),
		op->name,
		op->type & FILE_OP_ISDIR ? "dir":"file"
	);
	if (op->type & FILE_OP_ISDIR)
		return;

	m->id = fs->id;
	m->key = fs->key;
	m->opcode = MESSAGE_WRITE;
	m->type   = 0;
	strcpy(m->name, op->name);
	m->offset = op->offset;
	m->count  = 0;
	m->length = file_size(tmp_fname);

	fd = fopen(tmp_fname, "rb");
	if (NULL != fd && 0 == fseek(fd, op->offset, SEEK_SET)) {
		len = fread(m->data, 1, sizeof(m->data), fd);
		m->count  = len;
	} else {
		perror(tmp_fname);
		if (NULL != fd)
			fclose(fd);
	}
	if (NULL != fd)
		fclose(fd);

	socket_sendto(fs->sock, m, sizeof(*m), fs->group_addr);
}


void fs_remote_create(struct shfs *fs, struct shfs_file_op *op)
{
	char tmp_fname[1024];
	char main_fname[1024];
	char trash_fname[1024];

	fprintf(stderr, "FS REMOTE create %d bytes '%s' [%s]\n",
		op->length,
		op->name,
		op->type & FILE_OP_ISDIR ? "dir":"file"
	);

	sprintf(tmp_fname, "%s/%s", fs->tmpdir, op->name);
	sprintf(main_fname, "%s/%s", fs->maindir, op->name);
	sprintf(trash_fname, "%s/%s", fs->trashdir, op->name);

	if (op->type & FILE_OP_ISDIR) {
		if (!file_exists(tmp_fname)) file_mkdir(tmp_fname);
		if (!file_exists(main_fname)) file_mkdir(main_fname);
		if (!file_exists(trash_fname)) file_mkdir(trash_fname);
	} else {
		fd_list_open(fs, op->name, op->length);
		fd_list_request_next_offset(fs, op->name);
	}
}


void fs_remote_write(struct shfs *fs, struct shfs_file_op *op)
{
	fprintf(stderr, "FS REMOTE write %d+%d/%d '%s' [%s]\n",
		(int)op->offset,
		(int)op->count,
		(int)op->length,
		op->name,
		op->type & FILE_OP_ISDIR ? "dir":"file"
	);
	if (op->type & FILE_OP_ISDIR)
		return;
	if (!fd_list_has(fs, op->name))
		return;

	if (op->length <= op->offset || !op->count) {
		fd_list_close(fs, op->name);
		return;
	}

	fd_list_write(fs, op->name, op->offset, op->data, op->count);
	fd_list_request_next_offset(fs, op->name);
}


void fs_remote_delete(struct shfs *fs, struct shfs_file_op *op)
{
	char tmp_fname[1024];
	char main_fname[1024];
	char trash_fname[1024];

	fprintf(stderr, "FS REMOTE delete '%s' [%s]\n",
		op->name,
		op->type & FILE_OP_ISDIR ? "dir":"file"
	);

	sprintf(tmp_fname, "%s/%s", fs->tmpdir, op->name);
	sprintf(main_fname, "%s/%s", fs->maindir, op->name);
	sprintf(trash_fname, "%s/%s", fs->trashdir, op->name);

	if (file_exists(main_fname)) file_rm(main_fname);
	if (file_exists(trash_fname)) file_rm(trash_fname);
	if (file_exists(tmp_fname))
		file_mv(tmp_fname, trash_fname);
}


void fs_remote_rename(struct shfs *fs, struct shfs_file_op *op)
{
	char old_tmpdir_fname[1024];
	char new_tmpdir_fname[1024];
	char old_maindir_fname[1024];
	char new_maindir_fname[1024];

	fprintf(stderr, "FS REMOTE rename '%s' -> %s [%s]\n",
		op->name,
		op->toname,
		op->type & FILE_OP_ISDIR ? "dir":"file"
	);

	sprintf(old_tmpdir_fname, "%s/%s", fs->tmpdir, op->name);
	sprintf(new_tmpdir_fname, "%s/%s", fs->tmpdir, op->toname);
	sprintf(old_maindir_fname, "%s/%s", fs->maindir, op->name);
	sprintf(new_maindir_fname, "%s/%s", fs->maindir, op->toname);

	if (file_exists(new_tmpdir_fname)) file_rm(new_tmpdir_fname);
	if (file_exists(new_maindir_fname)) file_rm(new_maindir_fname);
	file_mv(old_tmpdir_fname, new_tmpdir_fname);
	file_mv(old_maindir_fname, new_maindir_fname);
}


void fs_backup(struct shfs *fs, struct shfs_file_op *op)
{
	char tmp_fname[1024];
	char main_fname[1024];
	char trash_fname[1024];

	fprintf(stderr, "FS LOCAL  backup '%s' [%s]\n",
		op->name,
		op->type & FILE_OP_ISDIR ? "dir":"file"
	);

	if (fd_list_has(fs, op->name)) {
		fd_list_close(fs, op->name);
		fd_list_remove(fs, op->name);
		fprintf(stderr, "FS LOCAL  finish-write '%s' [%s]\n",
			op->name,
			op->type & FILE_OP_ISDIR ? "dir":"file"
		);
		return;
	}

	sprintf(tmp_fname, "%s/%s", fs->tmpdir, op->name);
	sprintf(main_fname, "%s/%s", fs->maindir, op->name);
	sprintf(trash_fname, "%s/%s", fs->trashdir, op->name);

	if (file_isdir(main_fname) || (op->type & FILE_OP_ISDIR)) {
		if (!file_exists(tmp_fname))   file_mkdir(tmp_fname);
		if (!file_exists(trash_fname)) file_mkdir(trash_fname);
	} else if (!file_isdir(tmp_fname)) {
		file_cp(main_fname, tmp_fname);
	}

	queue_enqueue(fs->queue_send, op);
}


void fs_delete(struct shfs *fs, struct shfs_file_op *op)
{
	char tmp_fname[1024];
	char trash_fname[1024];

	fprintf(stderr, "FS LOCAL  delete '%s' [%s]\n",
		op->name,
		op->type & FILE_OP_ISDIR ? "dir":"file"
	);

	sprintf(tmp_fname, "%s/%s", fs->tmpdir, op->name);
	sprintf(trash_fname, "%s/%s", fs->trashdir, op->name);

	if (!file_exists(tmp_fname))
		return;

	if (op->type & FILE_OP_ISDIR) {
		file_rm(tmp_fname);
	} else if (file_isdir(trash_fname) && file_exists(tmp_fname)) {
		file_rm(tmp_fname);
	} else {
		file_backup_mv(tmp_fname, trash_fname);
	}

	queue_enqueue(fs->queue_send, op);
}


void fs_rename(struct shfs *fs, struct shfs_file_op *op)
{
	char old_tmpdir_fname[1024];
	char new_tmpdir_fname[1024];
	char old_maindir_fname[1024];
	char new_maindir_fname[1024];

	fprintf(stderr, "FS LOCAL  rename '%s' -> '%s' [%s]\n",
		op->name,
		op->toname,
		op->type & FILE_OP_ISDIR ? "dir":"file"
	);

	sprintf(old_tmpdir_fname, "%s/%s", fs->tmpdir, op->name);
	sprintf(new_tmpdir_fname, "%s/%s", fs->tmpdir, op->toname);
	sprintf(old_maindir_fname, "%s/%s", fs->maindir, op->name);
	sprintf(new_maindir_fname, "%s/%s", fs->maindir, op->toname);
	file_mv(old_tmpdir_fname, new_tmpdir_fname);
	file_mv(old_maindir_fname, new_maindir_fname);

	queue_enqueue(fs->queue_send, op);
}


void* fs_thread(void *param)
{
	struct shfs *fs = param;

	while (1) {
		struct shfs_file_op op_alloc;
		struct shfs_file_op *op = &op_alloc;
		int s;


		s = queue_dequeue(fs->queue_fs, op);
		if (-1 == s || 0 == op->type)
			return NULL;


		if (op->type & FILE_OP_REMOTE_READ) {
			fs_remote_read(fs, op);
			continue;
		}

		if (op->type & FILE_OP_REMOTE_CREATE) {
			fs_remote_create(fs, op);
			continue;
		}

		if (op->type & FILE_OP_REMOTE_WRITE) {
			fs_remote_write(fs, op);
			continue;
		}

		if (op->type & FILE_OP_REMOTE_DELETE) {
			fs_remote_delete(fs, op);
			continue;
		}

		if (op->type & FILE_OP_REMOTE_RENAME) {
			fs_remote_rename(fs, op);
			continue;
		}

		if (op->type & FILE_OP_BACKUP) {
			fs_backup(fs, op);
			continue;
		}

		if (op->type & FILE_OP_DELETE) {
			fs_delete(fs, op);
			continue;
		}

		if (op->type & FILE_OP_RENAME) {
			fs_rename(fs, op);
			continue;
		}

	}

	return NULL;
}
