/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDHT may be copied only under the terms of the GNU General
* Public License V3.  Please visit the FastDHT Home Page 
* http://www.csource.org/ for more detail.
**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include "shared_func.h"
#include "logger.h"
#include "hash.h"
#include "fdht_global.h"
#include "ini_file_reader.h"
#include "sockopt.h"
#include "sync.h"

#define BINLOG_COMPRESSED_INDEX_FILENAME  SYNC_BINLOG_FILE_PREFIX".compressed.index"

typedef struct
{
	int binlog_index;
	int binlog_fd;
	off_t binlog_offset;
} CompressReader;

typedef struct
{
	time_t timestamp;
	char op_type;
	int key_hash_code;  //key hash code
	FDHTKeyInfo key_info;
	int value_len;
	time_t expires;  //key expires, 0 for never expired
	off_t offset;
	int record_length;
} CompressRecord;

typedef struct
{
	off_t offset;
	int record_length;
} CompressRawRow;

typedef struct
{
	CompressReader *pReader;
	int dest_fd;
	int row_count;
	char *buff;
	int buff_size;
} CompressWalkArg;

static int g_binlog_index;

static int get_current_binlog_index();
static int get_binlog_compressed_index();
static int write_to_binlog_compressed_index(const int compressed_index);
static int compress_open_readable_binlog(CompressReader *pReader);
static int compress_binlog_read(CompressReader *pReader, CompressRecord *pRecord);
static int compress_binlog_file(CompressReader *pReader);

int main(int argc, char *argv[])
{
	CompressReader reader;
	char binlog_filepath[MAX_PATH_SIZE];
	int result;
	int start_index;
	int end_index;
	int index;

	if (argc < 3)
	{
		printf("Usage: %s <base_path> <binglog file index>\n" \
			"\t file index based 0, \n" \
			"\t \"all\" means compressing all binlog files\n" \
			"\t \"auto\" means compressing binglog " \
			"files automaticlly\n" \
			"\t index number means single binlog file\n", argv[0]);
		return EINVAL;
	}

	memset(&reader, 0, sizeof(reader));
	reader.binlog_fd = -1;

	snprintf(g_base_path, sizeof(g_base_path), "%s", argv[1]);
	chopPath(g_base_path);

	if (!fileExists(g_base_path))
	{
		printf("path %s not exist!\n", g_base_path);
		return ENOENT;
	}

	snprintf(binlog_filepath, sizeof(binlog_filepath), \
		"%s/data/sync", g_base_path);
	if (!fileExists(binlog_filepath))
	{
		printf("binlog path %s not exist!\n", binlog_filepath);
		return ENOENT;
	}

	if ((result=get_current_binlog_index()) != 0)
	{
		return result;
	}

	if (strcmp(argv[2], "all") == 0)
	{
		if (g_binlog_index == 0)
		{
			printf("Current binlog index: %d == 0, " \
				"can't compress!\n", g_binlog_index);
			return EINVAL;
		}

		start_index = 0;
		end_index = g_binlog_index - 1;
	}
	else if (strcmp(argv[2], "auto") == 0)
	{
		if ((result=get_binlog_compressed_index(&start_index)) != 0)
		{
			return result;
		}

		end_index = g_binlog_index - 1;
		if (start_index > end_index)
		{
			return 0;
		}
	}
	else
	{
		char *pEnd;
		pEnd = NULL;
		start_index = end_index = (int)strtol(argv[2], &pEnd, 10);
		if ((pEnd != NULL && *pEnd != '\0') || start_index < 0)
		{
			printf("Invalid binlog file index: %s\n", argv[2]);
			return EINVAL;
		}

		if (start_index >= g_binlog_index)
		{
			printf("The compress index: %d >= current binlog " \
				"index: %d, can't compress!\n", \
				start_index, g_binlog_index);
			return EINVAL;
		}
	}

	for (index=start_index; index<=end_index; index++)
	{
		reader.binlog_index = index;
		if ((result=compress_binlog_file(&reader)) != 0)
		{
			return result;
		}

		if (strcmp(argv[2], "auto") == 0)
		{
			if ((result=write_to_binlog_compressed_index(index + 1)) != 0)
			{
				return result;
			}
		}
	}

	return 0;
}

static int get_binlog_compressed_index(int *compressed_index)
{
	char full_filename[MAX_PATH_SIZE];
	char file_buff[64];
	int result;
	int bytes;
	int fd;

	snprintf(full_filename, sizeof(full_filename), \
		"%s/data/"SYNC_DIR_NAME"/%s", g_base_path, \
		BINLOG_COMPRESSED_INDEX_FILENAME);
	if ((fd=open(full_filename, O_RDONLY)) < 0)
	{
		*compressed_index = 0;
		if ((result=write_to_binlog_compressed_index(\
				*compressed_index)) != 0)
		{
			return result;
		}

		return 0;
	}

	bytes = read(fd, file_buff, sizeof(file_buff) - 1);
	close(fd);
	if (bytes <= 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"read file \"%s\" fail, bytes read: %d", \
			__LINE__, full_filename, bytes);
		return errno != 0 ? errno : EIO;
	}

	file_buff[bytes] = '\0';
	*compressed_index = atoi(file_buff);
	if (*compressed_index < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"in file \"%s\", compressed binlog index: %d < 0", \
			__LINE__, full_filename, *compressed_index);
		return EINVAL;
	}

	return 0;
}

static int write_to_binlog_compressed_index(const int compressed_index)
{
	char full_filename[MAX_PATH_SIZE];
	char buff[16];
	int fd;
	int len;

	snprintf(full_filename, sizeof(full_filename), \
			"%s/data/"SYNC_DIR_NAME"/%s", g_base_path, \
			BINLOG_COMPRESSED_INDEX_FILENAME);
	if ((fd=open(full_filename, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"open file \"%s\" fail, " \
			"errno: %d, error info: %s", \
			__LINE__, full_filename, \
			errno, strerror(errno));
		return errno != 0 ? errno : ENOENT;
	}

	len = sprintf(buff, "%d", compressed_index);
	if (write(fd, buff, len) != len)
	{
		logError("file: "__FILE__", line: %d, " \
			"write to file \"%s\" fail, " \
			"errno: %d, error info: %s",  \
			__LINE__, full_filename, \
			errno, strerror(errno));
		close(fd);
		return errno != 0 ? errno : EIO;
	}

	close(fd);
	return 0;
}

static int get_current_binlog_index()
{
	char full_filename[MAX_PATH_SIZE];
	char file_buff[64];
	int bytes;
	int fd;

	snprintf(full_filename, sizeof(full_filename), \
		"%s/data/"SYNC_DIR_NAME"/%s", g_base_path, \
		SYNC_BINLOG_INDEX_FILENAME);
	if ((fd=open(full_filename, O_RDONLY)) < 0)
	{
		printf("open file %s fail, errno: %d, error info: %s\n", \
			full_filename, errno, strerror(errno));
		return errno != 0 ? errno : EACCES;
	}

	bytes = read(fd, file_buff, sizeof(file_buff) - 1);
	close(fd);
	if (bytes <= 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"read file \"%s\" fail, bytes read: %d", \
			__LINE__, full_filename, bytes);
		return errno != 0 ? errno : EIO;
	}

	file_buff[bytes] = '\0';
	g_binlog_index = atoi(file_buff);
	if (g_binlog_index < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"in file \"%s\", binlog_index: %d < 0", \
			__LINE__, full_filename, g_binlog_index);
		return EINVAL;
	}

	return 0;
}

static char *compress_get_binlog_filename(CompressReader *pReader, \
		char *full_filename)
{
	static char buff[MAX_PATH_SIZE];

	if (full_filename == NULL)
	{
		full_filename = buff;
	}

	snprintf(full_filename, MAX_PATH_SIZE, \
			"%s/data/"SYNC_DIR_NAME"/"SYNC_BINLOG_FILE_PREFIX"" \
			SYNC_BINLOG_FILE_EXT_FMT, \
			g_base_path, pReader->binlog_index);
	return full_filename;
}

static int compress_open_readable_binlog(CompressReader *pReader)
{
	char full_filename[MAX_PATH_SIZE];

	if (pReader->binlog_fd >= 0)
	{
		close(pReader->binlog_fd);
	}

	compress_get_binlog_filename(pReader, full_filename);
	pReader->binlog_fd = open(full_filename, O_RDONLY);
	if (pReader->binlog_fd < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"open binlog file \"%s\" fail, " \
			"errno: %d, error info: %s", \
			__LINE__, full_filename, \
			errno, strerror(errno));
		return errno != 0 ? errno : ENOENT;
	}
	pReader->binlog_offset = 0;

	return 0;
}

#define CHECK_FIELD_VALUE(pRecord, value, max_length, caption) \
	if (value < 0) \
	{ \
		logError("file: "__FILE__", line: %d, " \
			"item \"%s\" in binlog file \"%s\" " \
			"is invalid, file offset: "INT64_PRINTF_FORMAT", " \
			"%s: %d <= 0", __LINE__, caption, \
			compress_get_binlog_filename(pReader, NULL), \
			pReader->binlog_offset, caption, value); \
		return EINVAL; \
	} \
	if (value > max_length) \
	{ \
		logError("file: "__FILE__", line: %d, " \
			"item \"%s\" in binlog file \"%s\" " \
			"is invalid, file offset: "INT64_PRINTF_FORMAT", " \
			"%s: %d > %d", __LINE__, caption, \
			compress_get_binlog_filename(pReader, NULL), \
			pReader->binlog_offset, caption, value, max_length); \
		return EINVAL; \
	} \


static int compress_binlog_read(CompressReader *pReader, CompressRecord *pRecord)
{
	char buff[BINLOG_FIX_FIELDS_LENGTH + FDHT_MAX_FULL_KEY_LEN + 2];
	char *p;
	int read_bytes;
	int full_key_len;
	int nItem;
	time_t *ptTimestamp;
	time_t *ptExpires;
	int *piTimestamp;
	int *piExpires;

	read_bytes = read(pReader->binlog_fd, buff, BINLOG_FIX_FIELDS_LENGTH);
	if (read_bytes == 0)  //end of file
	{
		return ENOENT;

	}

	if (read_bytes < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"read from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"errno: %d, error info: %s", __LINE__, \
			compress_get_binlog_filename(pReader, NULL), \
			pReader->binlog_offset, errno, strerror(errno));
		return errno != 0 ? errno : EIO;
	}

	if (read_bytes != BINLOG_FIX_FIELDS_LENGTH)
	{
		logError("file: "__FILE__", line: %d, " \
			"read from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"read bytes: %d != %d", \
			__LINE__, compress_get_binlog_filename(pReader, NULL),\
			pReader->binlog_offset, read_bytes, \
			BINLOG_FIX_FIELDS_LENGTH);
		return EINVAL;
	}

	*(buff + read_bytes) = '\0';
	ptTimestamp = &(pRecord->timestamp);
	ptExpires = &(pRecord->expires);
	piTimestamp = (int *)ptTimestamp;
	piExpires = (int *)ptExpires;
	if ((nItem=sscanf(buff, "%10d %c %10d %10d %4d %4d %4d %10d ", \
			piTimestamp, &(pRecord->op_type), \
			&(pRecord->key_hash_code), piExpires, \
			&(pRecord->key_info.namespace_len), \
			&(pRecord->key_info.obj_id_len), \
			&(pRecord->key_info.key_len), \
			&(pRecord->value_len))) != 8)
	{
		logError("file: "__FILE__", line: %d, " \
			"data format invalid, binlog file: %s, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"read item: %d != 6", \
			__LINE__, compress_get_binlog_filename(pReader, NULL),\
			pReader->binlog_offset, nItem);
		return EINVAL;
	}

	CHECK_FIELD_VALUE(pRecord, pRecord->key_info.namespace_len, \
			FDHT_MAX_NAMESPACE_LEN, "namespace length")

	CHECK_FIELD_VALUE(pRecord, pRecord->key_info.obj_id_len, \
			FDHT_MAX_OBJECT_ID_LEN, "object ID length")

	CHECK_FIELD_VALUE(pRecord, pRecord->key_info.key_len, \
			FDHT_MAX_SUB_KEY_LEN, "key length")

	if (pRecord->value_len < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"item \"value length\" in binlog file \"%s\" " \
			"is invalid, file offset: "INT64_PRINTF_FORMAT", " \
			"value length: %d < 0", \
			__LINE__, compress_get_binlog_filename(pReader, NULL), \
			pReader->binlog_offset, pRecord->value_len);
		return EINVAL;
	}

	full_key_len = pRecord->key_info.namespace_len + 1 + \
			pRecord->key_info.obj_id_len + 1 + \
			pRecord->key_info.key_len + 1;
	read_bytes = read(pReader->binlog_fd, buff, full_key_len);
	if (read_bytes < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"read from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"errno: %d, error info: %s", __LINE__, \
			compress_get_binlog_filename(pReader, NULL), \
			pReader->binlog_offset, errno, strerror(errno));
		return errno != 0 ? errno : EIO;
	}
	if (read_bytes != full_key_len)
	{
		logError("file: "__FILE__", line: %d, " \
			"read from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"read bytes: %d != %d", \
			__LINE__, compress_get_binlog_filename(pReader, NULL),\
			pReader->binlog_offset, read_bytes, full_key_len);
		return EINVAL;
	}

	p = buff;
	if (pRecord->key_info.namespace_len > 0)
	{
		memcpy(pRecord->key_info.szNameSpace, p, \
			pRecord->key_info.namespace_len);
		p += pRecord->key_info.namespace_len;
	}
	p++;

	if (pRecord->key_info.obj_id_len > 0)
	{
		memcpy(pRecord->key_info.szObjectId, p, \
			pRecord->key_info.obj_id_len);
		p += pRecord->key_info.obj_id_len;
	}
	p++;

	memcpy(pRecord->key_info.szKey, p, \
		pRecord->key_info.key_len);
	
	if (lseek(pReader->binlog_fd, pRecord->value_len + 1, SEEK_CUR) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"lseek from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"errno: %d, error info: %s", __LINE__, \
			compress_get_binlog_filename(pReader, NULL), \
			pReader->binlog_offset, errno, strerror(errno));
		return errno != 0 ? errno : EIO;
	}

	pRecord->offset = pReader->binlog_offset;
	pRecord->record_length = CALC_RECORD_LENGTH((&(pRecord->key_info)), \
					pRecord->value_len);

	pReader->binlog_offset += pRecord->record_length;

	/*
	printf("timestamp=%d, op_type=%c, key len=%d, value len=%d, " \
		"record length=%d, offset=%d\n", \
		(int)pRecord->timestamp, pRecord->op_type, \
		pRecord->key_info.key_len, pRecord->value_len, \
		pRecord->record_length, (int)pReader->binlog_offset);
	*/

	return 0;
}

static int compress_write_to_binlog(const int index, const HashData *data, void *args)
{
	CompressWalkArg *pWalkArg;
	CompressRawRow *pRow;

	pWalkArg = (CompressWalkArg *)args;
	pWalkArg->row_count++;

	pRow = (CompressRawRow *)data->value;
	if (pRow->record_length > pWalkArg->buff_size)
	{
		char *pTmp;
		int new_size;

		new_size = pRow->record_length + 4 * 1024;
		pTmp = (char *)malloc(new_size);
		if (pTmp == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"malloc %d bytes fail, " \
				"errno: %d, error info: %s", __LINE__, \
				new_size, errno, strerror(errno));
			return errno != 0 ? errno : ENOMEM;
		}

		free(pWalkArg->buff);
		pWalkArg->buff = pTmp;
		pWalkArg->buff_size = new_size;
	}

	if (lseek(pWalkArg->pReader->binlog_fd, pRow->offset, SEEK_SET) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"lseek from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"errno: %d, error info: %s", __LINE__, \
			compress_get_binlog_filename(pWalkArg->pReader, NULL), \
			pRow->offset, errno, strerror(errno));
		return errno != 0 ? errno : EIO;
	}

	if (read(pWalkArg->pReader->binlog_fd, pWalkArg->buff, 
			pRow->record_length) != pRow->record_length)
	{
		logError("file: "__FILE__", line: %d, " \
			"read from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"errno: %d, error info: %s", __LINE__, \
			compress_get_binlog_filename(pWalkArg->pReader, NULL), \
			pRow->offset, errno, strerror(errno));
		return errno != 0 ? errno : EIO;
	}

	if (write(pWalkArg->dest_fd, pWalkArg->buff, 
			pRow->record_length) != pRow->record_length)
	{
		logError("file: "__FILE__", line: %d, " \
			"write to new binlog file \"%s.new\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"errno: %d, error info: %s", __LINE__, \
			compress_get_binlog_filename(pWalkArg->pReader, NULL), \
			pRow->offset, errno, strerror(errno));
		return errno != 0 ? errno : EIO;
	}

	return 0;
}

static int compress_binlog_file(CompressReader *pReader)
{
	int result;
	int row_count;
	CompressRawRow *rows;
	CompressRawRow *pRow;
	CompressRecord record;
	HashArray key_hash;
	CompressWalkArg walk_arg;
	char full_filename[MAX_PATH_SIZE];
	char new_filename[MAX_PATH_SIZE];
	time_t current_time;
	char full_key[FDHT_MAX_FULL_KEY_LEN];
	int full_key_len;
	char *p;

	memset(&record, 0, sizeof(record));
	if ((result=compress_open_readable_binlog(pReader)) != 0)
	{
		return result;
	}

	row_count = 0;
	while (1)
	{
		result = compress_binlog_read(pReader, &record);
		if (result != 0)
		{
			break;
		}

		row_count++;
	}

	if (result != 0 && result != ENOENT)
	{
		return result;
	}

	if (row_count == 0)
	{
		return result;
	}

	rows = (CompressRawRow *)malloc(sizeof(CompressRawRow) * row_count);
	if (rows == NULL)
	{
		printf("malloc %d bytes fail, errno: %d, error info: %s\n", 
			sizeof(CompressRawRow) * row_count, 
			errno, strerror(errno));
		return errno != 0 ? errno : ENOMEM;
	}

	pReader->binlog_offset = 0;
	if (lseek(pReader->binlog_fd, 0, SEEK_SET) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"lseek from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"errno: %d, error info: %s", __LINE__, \
			compress_get_binlog_filename(pReader, NULL), \
			pReader->binlog_offset, errno, strerror(errno));
		return errno != 0 ? errno : EIO;
	}

	if ((result=hash_init(&key_hash, PJWHash, row_count, 1.0)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"hash_init fail, errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
		return result;
	}

	current_time = time(NULL);
	pRow = rows;
	while (1)
	{
		result = compress_binlog_read(pReader, &record);
		if (result != 0)
		{
			break;
		}

		FDHT_PACK_FULL_KEY(record.key_info, full_key, full_key_len, p)
		if ((record.expires != FDHT_EXPIRES_NEVER && \
			record.expires < current_time) || \
		     (record.op_type == FDHT_OP_TYPE_SOURCE_DEL || \
		      record.op_type == FDHT_OP_TYPE_REPLICA_DEL))
		{
			if ((result=hash_delete(&key_hash, full_key, \
					full_key_len)) < 0)
			{
				logError("file: "__FILE__", line: %d, " \
					"hash_insert fail, errno: %d, error info: %s", \
					__LINE__, result, strerror(result));
				break;
			}
		}
		else
		{
			pRow->offset = record.offset;
			pRow->record_length = record.record_length;

			if ((result=hash_insert(&key_hash, full_key, \
					full_key_len, pRow)) < 0)
			{
				logError("file: "__FILE__", line: %d, " \
					"hash_insert fail, errno: %d, error info: %s", \
					__LINE__, result, strerror(result));
				break;
			}

			pRow++;
		}
	}

	if (result != 0 && result != ENOENT)
	{
		return result;
	}

	result = 0;
	memset(&walk_arg, 0, sizeof(walk_arg));
	walk_arg.pReader = pReader;
	walk_arg.buff_size = 64 * 1024;
	walk_arg.buff = (char *)malloc(walk_arg.buff_size);
	if (walk_arg.buff == NULL)
	{
		logError("file: "__FILE__", line: %d, " \
			"malloc %d bytes fail, " \
			"errno: %d, error info: %s", __LINE__, \
			walk_arg.buff_size, errno, strerror(errno));
		return errno != 0 ? errno : ENOMEM;
	}

	if (lseek(pReader->binlog_fd, 0, SEEK_SET) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"lseek from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"errno: %d, error info: %s", __LINE__, \
			compress_get_binlog_filename(pReader, NULL), \
			pReader->binlog_offset, errno, strerror(errno));
		return errno != 0 ? errno : EIO;
	}

	compress_get_binlog_filename(pReader, full_filename);
	snprintf(new_filename, sizeof(new_filename), "%s.new", full_filename);
	if ((walk_arg.dest_fd=open(new_filename, O_WRONLY | O_CREAT | 
			O_TRUNC, 0666)) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"open file %s fail, errno: %d, error info: %s", \
			__LINE__, new_filename, errno, strerror(errno));
		return errno != 0 ? errno : EACCES;
	}

	result = hash_walk(&key_hash, compress_write_to_binlog, &walk_arg);
	
	close(walk_arg.dest_fd);
	close(pReader->binlog_fd);
	pReader->binlog_fd = -1;

	free(walk_arg.buff);
	hash_destroy(&key_hash);
	free(rows);

	if (result != 0)
	{
		return result;
	}

	if (rename(new_filename, full_filename) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"rename file from %s to %s fail, " \
			"errno: %d, error info: %s", \
			__LINE__, new_filename, full_filename, 
			errno, strerror(errno));
		return errno != 0 ? errno : EACCES;
	}

	printf("binlog: %s, row count before compress=%d, after compress=%d\n" \
		, full_filename, row_count, walk_arg.row_count);

	return result;
}

