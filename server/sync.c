/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/

//sync.c

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include "fdht_define.h"
#include "logger.h"
#include "sockopt.h"
#include "shared_func.h"
#include "ini_file_reader.h"
#include "hash.h"
#include "global.h"
#include "func.h"
#include "sync.h"

#define DATA_DIR_INITED_FILENAME	".sync_init_flag"
#define INIT_ITEM_SERVER_JOIN_TIME	"server_join_time"
#define INIT_ITEM_SYNC_OLD_DONE		"sync_old_done"
#define INIT_ITEM_SYNC_SRC_SERVER	"sync_src_server"
#define INIT_ITEM_SYNC_SRC_PORT		"sync_src_port"
#define INIT_ITEM_SYNC_UNTIL_TIMESTAMP	"sync_until_timestamp"
#define INIT_ITEM_SYNC_DONE_TIMESTAMP	"sync_done_timestamp"


#define SYNC_BINLOG_FILE_MAX_SIZE	1024 * 1024 * 1024
#define SYNC_BINLOG_FILE_PREFIX		"binlog"
#define SYNC_BINLOG_INDEX_FILENAME	SYNC_BINLOG_FILE_PREFIX".index"
#define SYNC_MARK_FILE_EXT		".mark"
#define SYNC_BINLOG_FILE_EXT_FMT	".%03d"
#define SYNC_DIR_NAME			"sync"
#define MARK_ITEM_BINLOG_FILE_INDEX	"binlog_index"
#define MARK_ITEM_BINLOG_FILE_OFFSET	"binlog_offset"
#define MARK_ITEM_NEED_SYNC_OLD		"need_sync_old"
#define MARK_ITEM_SYNC_OLD_DONE		"sync_old_done"
#define MARK_ITEM_UNTIL_TIMESTAMP	"until_timestamp"
#define MARK_ITEM_SCAN_ROW_COUNT	"scan_row_count"
#define MARK_ITEM_SYNC_ROW_COUNT	"sync_row_count"

int g_binlog_fd = -1;
int g_binlog_index = 0;
static off_t binlog_file_size = 0;

int g_fdht_sync_thread_count = 0;

static pthread_mutex_t sync_thread_lock;

/* save sync thread ids */
static pthread_t *sync_tids = NULL;

static char binlog_write_cache_buff[256 * 1024];
static char *pbinlog_write_cache_current = binlog_write_cache_buff;

static int fdht_write_to_mark_file(BinLogReader *pReader);
static int fdht_binlog_reader_skip(BinLogReader *pReader);
static void fdht_reader_destroy(BinLogReader *pReader);
static int fdht_sync_thread_start(const FDHTGroupServer *pDestServer);
static int fdht_binlog_fsync(const bool bNeedLock);



#define CALC_RECORD_LENGTH(key_len, value_len)  BINLOG_FIX_FIELDS_LENGTH + \
			key_len + 1 + value_len + 1
/**
* request body format:
*      server port : 4 bytes
* response body format:
*      none
*/
static int fdht_report_sync_done(FDHTServerInfo *pDestServer)
{
	int result;
	ProtoHeader *pHeader;
	char out_buff[sizeof(ProtoHeader) + 8];
	char in_buff[1];
	char *pInBuff;
	int in_bytes;

	memset(out_buff, 0, sizeof(out_buff));
	pHeader = (ProtoHeader *)out_buff;
	pHeader->cmd = FDHT_PROTO_CMD_SYNC_NOTIFY;
	int2buff(4, pHeader->pkg_len);
	int2buff(g_server_port, out_buff + sizeof(ProtoHeader));

	if ((result=tcpsenddata(pDestServer->sock, out_buff, \
		sizeof(ProtoHeader) + 4, g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"send data to server %s:%d fail, " \
			"errno: %d, error info: %s", __LINE__, \
			pDestServer->ip_addr, pDestServer->port, \
			result, strerror(result));
		return result;
	}

	pInBuff = in_buff;
	if ((result=fdht_recv_response(pDestServer, &pInBuff, \
		0, &in_bytes)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"recv data from server %s:%d fail, " \
			"errno: %d, error info: %s", __LINE__, \
			pDestServer->ip_addr, pDestServer->port, \
			result, strerror(result));
		return result;
	}

	logInfo("file: "__FILE__", line: %d, " \
		"sync old data to dest server: %s:%d done.", \
		__LINE__, pDestServer->ip_addr, pDestServer->port);

	return 0;
}

/**
* request body format:
*      server port : 4 bytes
*      sync_old_done: 1 byte
*      update count: 8 bytes
* response body format:
*      sync_old_done: 1 byte
*      sync_src_ip_addr: IP_ADDRESS_SIZE bytes
*      sync_src_port:  4 bytes
*      sync_until_timestamp: 4 bytes
*/
static int fdht_sync_req(FDHTServerInfo *pDestServer, BinLogReader *pReader)
{
	int result;
	ProtoHeader *pHeader;
	char out_buff[sizeof(ProtoHeader) + 16];
	char in_buff[IP_ADDRESS_SIZE + 16];
	int in_bytes;
	char sync_src_ip_addr[IP_ADDRESS_SIZE];
	int sync_src_port;

	memset(out_buff, 0, sizeof(out_buff));
	pHeader = (ProtoHeader *)out_buff;
	pHeader->cmd = FDHT_PROTO_CMD_SYNC_REQ;
	int2buff(13, pHeader->pkg_len);

	int2buff(g_server_port, out_buff + sizeof(ProtoHeader));
	*(out_buff + sizeof(ProtoHeader) + 4) = g_sync_old_done;
	long2buff(g_server_stat.success_set_count+g_server_stat.success_inc_count
		+ g_server_stat.success_delete_count,
		out_buff + sizeof(ProtoHeader) + 5);

	if ((result=tcpsenddata(pDestServer->sock, out_buff, \
		sizeof(ProtoHeader) + 13, g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"send data to server %s:%d fail, " \
			"errno: %d, error info: %s", __LINE__, \
			pDestServer->ip_addr, pDestServer->port, \
			result, strerror(result));
		return result;
	}

	if ((result=fdht_recv_header(pDestServer, &in_bytes)) != 0)
	{
		return result;
	}

	if (in_bytes != IP_ADDRESS_SIZE + 9)
	{
		logError("file: "__FILE__", line: %d, " \
			"recv data from server %s:%d fail, " \
			"body length != %d", __LINE__, \
			pDestServer->ip_addr, pDestServer->port, \
			IP_ADDRESS_SIZE + 9);
		return EINVAL;
	}

	if ((result=tcprecvdata(pDestServer->sock, in_buff, \
                       IP_ADDRESS_SIZE + 9, g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"recv data from server %s:%d fail, " \
			"errno: %d, error info: %s", __LINE__, \
			pDestServer->ip_addr, pDestServer->port, \
			result, strerror(result));
		return result;
	}

	pReader->sync_old_done = *in_buff;
	memcpy(sync_src_ip_addr, in_buff+1, IP_ADDRESS_SIZE - 1);
	sync_src_ip_addr[IP_ADDRESS_SIZE - 1] = '\0';
	sync_src_port = buff2int(in_buff+1+IP_ADDRESS_SIZE);
	pReader->until_timestamp = buff2int(in_buff+1+IP_ADDRESS_SIZE+4);

	if (is_local_host_ip(sync_src_ip_addr) && \
		sync_src_port == g_server_port)
	{
		pReader->need_sync_old = true;
	}
	else
	{
		pReader->need_sync_old = false;
	}

	formatDatetime(pReader->until_timestamp, "%Y-%m-%d %H:%M:%S", \
			out_buff, sizeof(out_buff));
	logInfo("file: "__FILE__", line: %d, " \
		"sync dest server: %s:%d, src server: %s:%d, " \
		"until_timestamp=%s, sync_old_done=%d", \
		__LINE__, pDestServer->ip_addr, pDestServer->port, \
		sync_src_ip_addr, sync_src_port, \
		out_buff, pReader->sync_old_done);

	return 0;
}

static int fdht_sync_set(FDHTServerInfo *pDestServer, \
			const BinLogRecord *pRecord)
{
	int group_id;

	group_id = PJWHash(pRecord->key.data,pRecord->key.length)%g_group_count;
	return fdht_client_set(pDestServer, pRecord->timestamp, \
		FDHT_PROTO_CMD_SYNC_SET, group_id, \
		pRecord->key.data, pRecord->key.length, \
		pRecord->value.data, pRecord->value.length);
}

static int fdht_sync_del(FDHTServerInfo *pDestServer, \
			const BinLogRecord *pRecord)
{
	int group_id;

	group_id = PJWHash(pRecord->key.data,pRecord->key.length)%g_group_count;
	return fdht_client_delete(pDestServer, pRecord->timestamp, \
		FDHT_PROTO_CMD_SYNC_DEL, group_id, \
		pRecord->key.data, pRecord->key.length);
}

#define STARAGE_CHECK_IF_NEED_SYNC_OLD(pReader, pRecord) \
	if ((!pReader->need_sync_old) || pReader->sync_old_done || \
		(pRecord->timestamp > pReader->until_timestamp)) \
	{ \
		return 0; \
	} \

static int fdht_sync_data(BinLogReader *pReader, \
			FDHTServerInfo *pDestServer, \
			const BinLogRecord *pRecord)
{
	int result;
	switch(pRecord->op_type)
	{
		case FDHT_OP_TYPE_SOURCE_SET:
			result = fdht_sync_set(pDestServer, pRecord);
			break;
		case FDHT_OP_TYPE_SOURCE_DEL:
			result = fdht_sync_del(pDestServer, pRecord);
			break;
		case FDHT_OP_TYPE_REPLICA_SET:
			STARAGE_CHECK_IF_NEED_SYNC_OLD(pReader, pRecord)
			result = fdht_sync_set(pDestServer, pRecord);
			break;
		case FDHT_OP_TYPE_REPLICA_DEL:
			STARAGE_CHECK_IF_NEED_SYNC_OLD(pReader, pRecord)
			result = fdht_sync_del(pDestServer, pRecord);
			break;
		default:
			return EINVAL;
	}

	if (result == 0)
	{
		pReader->sync_row_count++;
	}

	return result;
}

static int write_to_binlog_index()
{
	char full_filename[MAX_PATH_SIZE];
	char buff[16];
	int fd;
	int len;

	snprintf(full_filename, sizeof(full_filename), \
			"%s/data/"SYNC_DIR_NAME"/%s", g_base_path, \
			SYNC_BINLOG_INDEX_FILENAME);
	if ((fd=open(full_filename, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"open file \"%s\" fail, " \
			"errno: %d, error info: %s", \
			__LINE__, full_filename, \
			errno, strerror(errno));
		return errno != 0 ? errno : ENOENT;
	}

	len = sprintf(buff, "%d", g_binlog_index);
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

static char *get_writable_binlog_filename(char *full_filename)
{
	static char buff[MAX_PATH_SIZE];

	if (full_filename == NULL)
	{
		full_filename = buff;
	}

	snprintf(full_filename, MAX_PATH_SIZE, \
			"%s/data/"SYNC_DIR_NAME"/"SYNC_BINLOG_FILE_PREFIX"" \
			SYNC_BINLOG_FILE_EXT_FMT, \
			g_base_path, g_binlog_index);
	return full_filename;
}

static int open_next_writable_binlog()
{
	char full_filename[MAX_PATH_SIZE];

	fdht_sync_destroy();

	get_writable_binlog_filename(full_filename);
	if (fileExists(full_filename))
	{
		if (unlink(full_filename) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"unlink file \"%s\" fail, " \
				"errno: %d, error info: %s", \
				__LINE__, full_filename, \
				errno, strerror(errno));
			return errno != 0 ? errno : ENOENT;
		}

		logError("file: "__FILE__", line: %d, " \
			"binlog file \"%s\" already exists, truncate", \
			__LINE__, full_filename);
	}

	g_binlog_fd = open(full_filename, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (g_binlog_fd < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"open file \"%s\" fail, " \
			"errno: %d, error info: %s", \
			__LINE__, full_filename, \
			errno, strerror(errno));
		return errno != 0 ? errno : EACCES;
	}

	return 0;
}

static int create_sync_threads()
{
	FDHTGroupServer *pServer;
	FDHTGroupServer *pEnd;
	int result;

	pEnd = g_group_servers + g_group_server_count;
	for (pServer=g_group_servers; pServer<pEnd; pServer++)
	{
		//printf("%s:%d\n", pServer->ip_addr, pServer->port);
		if ((result=fdht_sync_thread_start(pServer)) != 0)
		{
			return result;
		}
	}

	return 0;
}

static int load_sync_init_data()
{
	IniItemInfo *items;
	int nItemCount;
	char *pValue;
	int result;
	char data_filename[MAX_PATH_SIZE];

	snprintf(data_filename, sizeof(data_filename), "%s/data/%s", \
			g_base_path, DATA_DIR_INITED_FILENAME);
	if (fileExists(data_filename))
	{
		if ((result=iniLoadItems(data_filename, \
				&items, &nItemCount)) \
			 != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"load from file \"%s\" fail, " \
				"error code: %d", \
				__LINE__, data_filename, result);
			return result;
		}
		
		pValue = iniGetStrValue(INIT_ITEM_SERVER_JOIN_TIME, \
				items, nItemCount);
		if (pValue == NULL)
		{
			iniFreeItems(items);
			logError("file: "__FILE__", line: %d, " \
				"in file \"%s\", item \"%s\" not exists", \
				__LINE__, data_filename, \
				INIT_ITEM_SERVER_JOIN_TIME);
			return ENOENT;
		}
		g_server_join_time = atoi(pValue);

		pValue = iniGetStrValue(INIT_ITEM_SYNC_OLD_DONE, \
				items, nItemCount);
		if (pValue == NULL)
		{
			iniFreeItems(items);
			logError("file: "__FILE__", line: %d, " \
				"in file \"%s\", item \"%s\" not exists", \
				__LINE__, data_filename, \
				INIT_ITEM_SYNC_OLD_DONE);
			return ENOENT;
		}
		g_sync_old_done = atoi(pValue);

		pValue = iniGetStrValue(INIT_ITEM_SYNC_SRC_SERVER, \
				items, nItemCount);
		if (pValue == NULL)
		{
			iniFreeItems(items);
			logError("file: "__FILE__", line: %d, " \
				"in file \"%s\", item \"%s\" not exists", \
				__LINE__, data_filename, \
				INIT_ITEM_SYNC_SRC_SERVER);
			return ENOENT;
		}
		snprintf(g_sync_src_ip_addr, sizeof(g_sync_src_ip_addr), \
				"%s", pValue);

		g_sync_src_port = iniGetIntValue(INIT_ITEM_SYNC_SRC_PORT, \
				items, nItemCount, 0);
		
		g_sync_until_timestamp = iniGetIntValue( \
				INIT_ITEM_SYNC_UNTIL_TIMESTAMP, \
				items, nItemCount, 0);

		g_sync_done_timestamp = iniGetIntValue( \
				INIT_ITEM_SYNC_DONE_TIMESTAMP, \
				items, nItemCount, 0);

		iniFreeItems(items);

		//printf("g_sync_old_done = %d\n", g_sync_old_done);
		//printf("g_sync_src_ip_addr = %s\n", g_sync_src_ip_addr);
		//printf("g_sync_until_timestamp = %d\n", g_sync_until_timestamp);
	}
	else
	{
		g_server_join_time = time(NULL);
		if ((result=write_to_sync_ini_file()) != 0)
		{
			return result;
		}
	}

	return 0;
}

int fdht_sync_init()
{
	char data_path[MAX_PATH_SIZE];
	char sync_path[MAX_PATH_SIZE];
	char full_filename[MAX_PATH_SIZE];
	char file_buff[64];
	int bytes;
	int result;
	int fd;

	snprintf(data_path, sizeof(data_path), \
			"%s/data", g_base_path);
	if (!fileExists(data_path))
	{
		if (mkdir(data_path, 0755) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"mkdir \"%s\" fail, " \
				"errno: %d, error info: %s", \
				__LINE__, data_path, \
				errno, strerror(errno));
			return errno != 0 ? errno : ENOENT;
		}
	}

	snprintf(sync_path, sizeof(sync_path), \
			"%s/"SYNC_DIR_NAME, data_path);
	if (!fileExists(sync_path))
	{
		if (mkdir(sync_path, 0755) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"mkdir \"%s\" fail, " \
				"errno: %d, error info: %s", \
				__LINE__, sync_path, \
				errno, strerror(errno));
			return errno != 0 ? errno : ENOENT;
		}
	}

	snprintf(full_filename, sizeof(full_filename), \
			"%s/%s", sync_path, SYNC_BINLOG_INDEX_FILENAME);
	if ((fd=open(full_filename, O_RDONLY)) >= 0)
	{
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
	}
	else
	{
		g_binlog_index = 0;
		if ((result=write_to_binlog_index()) != 0)
		{
			return result;
		}
	}

	get_writable_binlog_filename(full_filename);
	g_binlog_fd = open(full_filename, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (g_binlog_fd < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"open file \"%s\" fail, " \
			"errno: %d, error info: %s", \
			__LINE__, full_filename, \
			errno, strerror(errno));
		return errno != 0 ? errno : EACCES;
	}

	binlog_file_size = lseek(g_binlog_fd, 0, SEEK_END);
	if (binlog_file_size < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"ftell file \"%s\" fail, " \
			"errno: %d, error info: %s", \
			__LINE__, full_filename, \
			errno, strerror(errno));
		fdht_sync_destroy();
		return errno != 0 ? errno : EIO;
	}

	/*
	//printf("full_filename=%s, binlog_file_size=%d\n", \
			full_filename, binlog_file_size);
	*/
	
	if ((result=init_pthread_lock(&sync_thread_lock)) != 0)
	{
		return result;
	}

	load_local_host_ip_addrs();

	if ((result=load_sync_init_data()) != 0)
	{
		return result;
	}

	if ((result=create_sync_threads()) != 0)
	{
		return result;
	}

	return 0;
}

int fdht_sync_destroy()
{
	int result;
	if (g_binlog_fd >= 0)
	{
		fdht_binlog_fsync(true);
		close(g_binlog_fd);
		g_binlog_fd = -1;
	}

	if ((result=pthread_mutex_destroy(&sync_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_destroy fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
		return result;
	}


	return 0;
}

int kill_fdht_sync_threads()
{
	int result;

	if (sync_tids != NULL)
	{
		result = kill_work_threads(sync_tids, \
				g_fdht_sync_thread_count);

		free(sync_tids);
		sync_tids = NULL;
	}
	else
	{
		result = 0;
	}

	return result;
}

static char *get_binlog_readable_filename(BinLogReader *pReader, \
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

static int fdht_open_readable_binlog(BinLogReader *pReader)
{
	char full_filename[MAX_PATH_SIZE];

	if (pReader->binlog_fd >= 0)
	{
		close(pReader->binlog_fd);
	}

	get_binlog_readable_filename(pReader, full_filename);
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

	if (pReader->binlog_offset > 0 && \
	    lseek(pReader->binlog_fd, pReader->binlog_offset, SEEK_SET) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"seek binlog file \"%s\" fail, file offset="INT64_PRINTF_FORMAT", " \
			"errno: %d, error info: %s", \
			__LINE__, full_filename, pReader->binlog_offset, \
			errno, strerror(errno));

		close(pReader->binlog_fd);
		pReader->binlog_fd = -1;
		return errno != 0 ? errno : ESPIPE;
	}

	return 0;
}

static char *get_mark_filename(const void *pArg, \
			char *full_filename)
{
	const BinLogReader *pReader;
	static char buff[MAX_PATH_SIZE];

	pReader = (const BinLogReader *)pArg;
	if (full_filename == NULL)
	{
		full_filename = buff;
	}

	snprintf(full_filename, MAX_PATH_SIZE, \
			"%s/data/"SYNC_DIR_NAME"/%s_%d%s", g_base_path, \
			pReader->ip_addr, pReader->port, SYNC_MARK_FILE_EXT);
	return full_filename;
}

static int fdht_reader_sync_init(FDHTServerInfo *pDestServer, BinLogReader *pReader)
{
	int result;

	result = 0;
	while (g_continue_flag)
	{
		result = fdht_sync_req(pDestServer, pReader);
		if (result == 0)
		{
			break;
		}

		if (!(result == EAGAIN || result == ENOENT))
		{
			return result;
		}

		sleep(g_heart_beat_interval);
	}

	if (!g_continue_flag)
	{
		return result != 0 ? result : ENOENT;
	}

	return 0;
}

static int fdht_reader_init(FDHTServerInfo *pDestServer, \
			BinLogReader *pReader)
{
	char full_filename[MAX_PATH_SIZE];
	IniItemInfo *items;
	int nItemCount;
	int result;
	bool bFileExist;

	memset(pReader, 0, sizeof(BinLogReader));
	pReader->mark_fd = -1;
	pReader->binlog_fd = -1;

	strcpy(pReader->ip_addr, pDestServer->ip_addr);
	pReader->port = pDestServer->port;

	get_mark_filename(pReader, full_filename);
	bFileExist = fileExists(full_filename);
	if (bFileExist)
	{
		if ((result=iniLoadItems(full_filename, &items, &nItemCount)) \
			 != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"load from mark file \"%s\" fail, " \
				"error code: %d", \
				__LINE__, full_filename, result);
			return result;
		}

		if (nItemCount < 7)
		{
			iniFreeItems(items);
			logError("file: "__FILE__", line: %d, " \
				"in mark file \"%s\", item count: %d < 7", \
				__LINE__, full_filename, nItemCount);
			return ENOENT;
		}

		pReader->binlog_index = iniGetIntValue( \
				MARK_ITEM_BINLOG_FILE_INDEX, \
				items, nItemCount, -1);
		pReader->binlog_offset = iniGetInt64Value( \
				MARK_ITEM_BINLOG_FILE_OFFSET, \
				items, nItemCount, -1);
		pReader->need_sync_old = iniGetBoolValue(   \
				MARK_ITEM_NEED_SYNC_OLD, \
				items, nItemCount);
		pReader->sync_old_done = iniGetBoolValue(  \
				MARK_ITEM_SYNC_OLD_DONE, \
				items, nItemCount);
		pReader->until_timestamp = iniGetIntValue( \
				MARK_ITEM_UNTIL_TIMESTAMP, \
				items, nItemCount, -1);
		pReader->scan_row_count = iniGetInt64Value( \
				MARK_ITEM_SCAN_ROW_COUNT, \
				items, nItemCount, 0);
		pReader->sync_row_count = iniGetInt64Value( \
				MARK_ITEM_SYNC_ROW_COUNT, \
				items, nItemCount, 0);

		if (pReader->binlog_index < 0)
		{
			iniFreeItems(items);
			logError("file: "__FILE__", line: %d, " \
				"in mark file \"%s\", " \
				"binlog_index: %d < 0", \
				__LINE__, full_filename, \
				pReader->binlog_index);
			return EINVAL;
		}
		if (pReader->binlog_offset < 0)
		{
			iniFreeItems(items);
			logError("file: "__FILE__", line: %d, " \
				"in mark file \"%s\", " \
				"binlog_offset: "INT64_PRINTF_FORMAT" < 0", \
				__LINE__, full_filename, \
				pReader->binlog_offset);
			return EINVAL;
		}

		iniFreeItems(items);
	}
	else
	{
		if ((result=fdht_reader_sync_init(pDestServer, pReader)) != 0)
		{
			return result;
		}
	}

	pReader->last_write_row_count = pReader->scan_row_count;

	pReader->mark_fd = open(full_filename, O_WRONLY | O_CREAT, 0644);
	if (pReader->mark_fd < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"open mark file \"%s\" fail, " \
			"error no: %d, error info: %s", \
			__LINE__, full_filename, \
			errno, strerror(errno));
		return errno != 0 ? errno : ENOENT;
	}

	if ((result=fdht_open_readable_binlog(pReader)) != 0)
	{
		close(pReader->mark_fd);
		pReader->mark_fd = -1;
		return result;
	}

	if (!bFileExist)
	{
        	if (!pReader->need_sync_old && pReader->until_timestamp > 0)
		{
			if ((result=fdht_binlog_reader_skip(pReader)) != 0)
			{
				fdht_reader_destroy(pReader);
				return result;
			}
		}

		if ((result=fdht_write_to_mark_file(pReader)) != 0)
		{
			fdht_reader_destroy(pReader);
			return result;
		}
	}

	return 0;
}

static void fdht_reader_destroy(BinLogReader *pReader)
{
	if (pReader->mark_fd >= 0)
	{
		close(pReader->mark_fd);
		pReader->mark_fd = -1;
	}

	if (pReader->binlog_fd >= 0)
	{
		close(pReader->binlog_fd);
		pReader->binlog_fd = -1;
	}
}

static int fdht_write_to_mark_file(BinLogReader *pReader)
{
	char buff[256];
	int len;
	int result;

	len = sprintf(buff, 
		"%s=%d\n"  \
		"%s="INT64_PRINTF_FORMAT"\n"  \
		"%s=%d\n"  \
		"%s=%d\n"  \
		"%s=%d\n"  \
		"%s="INT64_PRINTF_FORMAT"\n"  \
		"%s="INT64_PRINTF_FORMAT"\n", \
		MARK_ITEM_BINLOG_FILE_INDEX, pReader->binlog_index, \
		MARK_ITEM_BINLOG_FILE_OFFSET, pReader->binlog_offset, \
		MARK_ITEM_NEED_SYNC_OLD, pReader->need_sync_old, \
		MARK_ITEM_SYNC_OLD_DONE, pReader->sync_old_done, \
		MARK_ITEM_UNTIL_TIMESTAMP, (int)pReader->until_timestamp, \
		MARK_ITEM_SCAN_ROW_COUNT, pReader->scan_row_count, \
		MARK_ITEM_SYNC_ROW_COUNT, pReader->sync_row_count);
	if ((result=fdht_write_to_fd(pReader->mark_fd, get_mark_filename, \
		pReader, buff, len)) == 0)
	{
		pReader->last_write_row_count = pReader->scan_row_count;
	}

	return result;
}

static int rewind_to_prev_rec_end(BinLogReader *pReader, \
			const int record_length)
{
	if (lseek(pReader->binlog_fd, -1 * record_length, \
			SEEK_CUR) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"seek binlog file \"%s\"fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"errno: %d, error info: %s", \
			__LINE__, get_binlog_readable_filename(pReader, NULL), \
			pReader->binlog_offset, \
			errno, strerror(errno));
		return errno != 0 ? errno : ENOENT;
	}

	return 0;
}

#define BINLOG_FIX_FIELDS_LENGTH  3 * 10 + 1 + 4 * 1

static int fdht_binlog_fsync(const bool bNeedLock)
{
	int result;
	int write_ret;
	int write_len;

	if (bNeedLock && (result=pthread_mutex_lock(&sync_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_lock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
	}

	write_len = pbinlog_write_cache_current - binlog_write_cache_buff;
	if (write_len == 0) //ignore
	{
		write_ret = 0;  //skip
	}
	else if (write(g_binlog_fd, binlog_write_cache_buff, \
		write_len) != write_len)
	{
		logError("file: "__FILE__", line: %d, " \
			"write to binlog file \"%s\" fail, " \
			"errno: %d, error info: %s",  \
			__LINE__, get_writable_binlog_filename(NULL), \
			errno, strerror(errno));
		write_ret = errno != 0 ? errno : EIO;
	}
	else if (fsync(g_binlog_fd) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"sync to binlog file \"%s\" fail, " \
			"errno: %d, error info: %s",  \
			__LINE__, get_writable_binlog_filename(NULL), \
			errno, strerror(errno));
		write_ret = errno != 0 ? errno : EIO;
	}
	else
	{
		binlog_file_size += write_len;
		if (binlog_file_size >= SYNC_BINLOG_FILE_MAX_SIZE)
		{
			g_binlog_index++;
			if ((write_ret=write_to_binlog_index()) == 0)
			{
				write_ret = open_next_writable_binlog();
			}

			binlog_file_size = 0;
			if (write_ret != 0)
			{
				fdht_terminate();
				logCrit("file: "__FILE__", line: %d, " \
					"open binlog file \"%s\" fail, " \
					"program exit!", \
					__LINE__, \
					get_writable_binlog_filename(NULL));
			}
		}
		else
		{
			write_ret = 0;
		}
	}

	//reset cache current pointer
	pbinlog_write_cache_current = binlog_write_cache_buff;

	if (bNeedLock && (result=pthread_mutex_unlock(&sync_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_unlock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
	}

	return write_ret;
}


static int fdht_binlog_direct_write(const time_t timestamp, const char op_type,\
		const char *pKey, const int key_len, \
		const char *pValue, const int value_len)
{
	char buff[64];
	int write_bytes;
	int result;

	write_bytes = sprintf(buff, "%10d %c %10d %10d ", \
			(int)timestamp, op_type, \
			key_len, value_len);
	if (write(g_binlog_fd, buff, write_bytes) != write_bytes)
	{
		logError("file: "__FILE__", line: %d, " \
			"write to binlog file \"%s\" fail, " \
			"errno: %d, error info: %s",  \
			__LINE__, get_writable_binlog_filename(NULL), \
			errno, strerror(errno));
		return errno != 0 ? errno : EIO;
	}

	if (write(g_binlog_fd, pKey, key_len) != key_len)
	{
		logError("file: "__FILE__", line: %d, " \
			"write to binlog file \"%s\" fail, " \
			"errno: %d, error info: %s",  \
			__LINE__, get_writable_binlog_filename(NULL), \
			errno, strerror(errno));
		return errno != 0 ? errno : EIO;
	}

	if (write(g_binlog_fd, " ", 1) != 1)
	{
		logError("file: "__FILE__", line: %d, " \
			"write to binlog file \"%s\" fail, " \
			"errno: %d, error info: %s",  \
			__LINE__, get_writable_binlog_filename(NULL), \
			errno, strerror(errno));
		return errno != 0 ? errno : EIO;
	}

	if (value_len > 0 && write(g_binlog_fd, pValue, \
		value_len) != value_len)
	{
		logError("file: "__FILE__", line: %d, " \
			"write to binlog file \"%s\" fail, " \
			"errno: %d, error info: %s",  \
			__LINE__, get_writable_binlog_filename(NULL), \
			errno, strerror(errno));
		return errno != 0 ? errno : EIO;
	}
	if (write(g_binlog_fd, "\n", 1) != 1)
	{
		logError("file: "__FILE__", line: %d, " \
			"write to binlog file \"%s\" fail, " \
			"errno: %d, error info: %s",  \
			__LINE__, get_writable_binlog_filename(NULL), \
			errno, strerror(errno));
		return errno != 0 ? errno : EIO;
	}

	if (fsync(g_binlog_fd) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"sync to binlog file \"%s\" fail, " \
			"errno: %d, error info: %s",  \
			__LINE__, get_writable_binlog_filename(NULL), \
			errno, strerror(errno));
		return errno != 0 ? errno : EIO;
	}

	binlog_file_size += BINLOG_FIX_FIELDS_LENGTH + key_len + 1 + \
				value_len + 1;

	if (binlog_file_size >= SYNC_BINLOG_FILE_MAX_SIZE)
	{
		g_binlog_index++;
		if ((result=write_to_binlog_index()) == 0)
		{
			result = open_next_writable_binlog();
		}

		binlog_file_size = 0;
		if (result != 0)
		{
			fdht_terminate();
			logCrit("file: "__FILE__", line: %d, " \
					"open binlog file \"%s\" fail, " \
				"program exit!", \
				__LINE__, \
				get_writable_binlog_filename(NULL));

			return result;
		}
	}

	return 0;
}

int fdht_binlog_write(const time_t timestamp, const char op_type, \
		const char *pKey, const int key_len, \
		const char *pValue, const int value_len)
{
	int record_len;
	int write_ret;
	int result;

	if ((result=pthread_mutex_lock(&sync_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_lock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
	}

	record_len = CALC_RECORD_LENGTH(key_len, value_len);
	if (record_len >= sizeof(binlog_write_cache_buff))
	{
		write_ret = fdht_binlog_direct_write(timestamp, op_type, \
				pKey, key_len, pValue, value_len);
		if ((result=pthread_mutex_unlock(&sync_thread_lock)) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"call pthread_mutex_unlock fail, " \
				"errno: %d, error info: %s", \
				__LINE__, result, strerror(result));
		}

		return write_ret;
	}

	//check if buff full
	if (sizeof(binlog_write_cache_buff) - (pbinlog_write_cache_current - \
					binlog_write_cache_buff) < record_len)
	{
		write_ret = fdht_binlog_fsync(false);  //sync to disk
	}
	else
	{
		write_ret = 0;
	}

	pbinlog_write_cache_current += sprintf(pbinlog_write_cache_current, \
			"%10d %c %10d %10d ", (int)timestamp, op_type, \
			key_len, value_len);

	memcpy(pbinlog_write_cache_current, pKey, key_len);
	pbinlog_write_cache_current += key_len;
	*pbinlog_write_cache_current++ = ' ';
	if (value_len > 0)
	{
		memcpy(pbinlog_write_cache_current, pValue, value_len);
		pbinlog_write_cache_current += value_len;
	}
	*pbinlog_write_cache_current++ = '\n';

	if ((result=pthread_mutex_unlock(&sync_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_unlock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
	}

	return write_ret;
}

static int fdht_binlog_read(BinLogReader *pReader, \
			BinLogRecord *pRecord, int *record_length)
{
	char buff[BINLOG_FIX_FIELDS_LENGTH + 1];
	int result;
	int read_bytes;
	int nItem;

	*record_length = 0;
	while (1)
	{
		read_bytes = read(pReader->binlog_fd, buff, \
				BINLOG_FIX_FIELDS_LENGTH);
		if (read_bytes == 0)  //end of file
		{
			if (pReader->binlog_index < g_binlog_index) //rotate
			{
				pReader->binlog_index++;
				pReader->binlog_offset = 0;
				if ((result=fdht_open_readable_binlog( \
						pReader)) != 0)
				{
					return result;
				}

				if ((result=fdht_write_to_mark_file( \
						pReader)) != 0)
				{
					return result;
				}

				continue;  //read next binlog
			}

			return ENOENT;

		}

		if (read_bytes < 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"read from binlog file \"%s\" fail, " \
				"file offset: "INT64_PRINTF_FORMAT", " \
				"errno: %d, error info: %s", __LINE__, \
				get_binlog_readable_filename(pReader, NULL), \
				pReader->binlog_offset, errno, strerror(errno));
			return errno != 0 ? errno : EIO;
		}

		break;
	}

	if (read_bytes != BINLOG_FIX_FIELDS_LENGTH)
	{
		if ((result=rewind_to_prev_rec_end(pReader, \
				read_bytes)) != 0)
		{
			return result;
		}

		logWarning("file: "__FILE__", line: %d, " \
			"read from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"read bytes: %d != %d", \
			__LINE__, get_binlog_readable_filename(pReader, NULL),\
			pReader->binlog_offset, read_bytes, \
			BINLOG_FIX_FIELDS_LENGTH);
		return ENOENT;
	}

	*(buff + read_bytes) = '\0';
	if ((nItem=sscanf(buff, "%10d %c %10d %10d ", \
			(int *)&(pRecord->timestamp), &(pRecord->op_type), \
			&(pRecord->key.length), &(pRecord->value.length))) != 4)
	{
		logError("file: "__FILE__", line: %d, " \
			"data format invalid, binlog file: %s, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"read item: %d != 4", \
			__LINE__, get_binlog_readable_filename(pReader, NULL),\
			pReader->binlog_offset, nItem);
		return ENOENT;
	}

	if (pRecord->key.length <= 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"item \"key length\" in binlog file \"%s\" " \
			"is invalid, file offset: "INT64_PRINTF_FORMAT", " \
			"key length: %d <= 0", \
			__LINE__, get_binlog_readable_filename(pReader, NULL), \
			pReader->binlog_offset, pRecord->key.length);
		return EINVAL;
	}
	if (pRecord->value.length < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"item \"value length\" in binlog file \"%s\" " \
			"is invalid, file offset: "INT64_PRINTF_FORMAT", " \
			"value length: %d < 0", \
			__LINE__, get_binlog_readable_filename(pReader, NULL), \
			pReader->binlog_offset, pRecord->value.length);
		return EINVAL;
	}

	if (pRecord->key.length > pRecord->key.size)
	{
		pRecord->key.size = pRecord->key.length + 64;
		pRecord->key.data = (char *)realloc(pRecord->key.data, \
						pRecord->key.size);
		if (pRecord->key.data == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"realloc %d bytes fail, " \
				"errno: %d, error info: %s", \
				pRecord->key.size, errno, strerror(errno));
			return errno != 0 ? errno : ENOMEM;
		}
	}

	read_bytes = read(pReader->binlog_fd, pRecord->key.data, \
			pRecord->key.length);
	if (read_bytes < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"read from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"errno: %d, error info: %s", __LINE__, \
			get_binlog_readable_filename(pReader, NULL), \
			pReader->binlog_offset, errno, strerror(errno));
		return errno != 0 ? errno : EIO;
	}
	if (read_bytes != pRecord->key.length)
	{
		if ((result=rewind_to_prev_rec_end(pReader, \
			BINLOG_FIX_FIELDS_LENGTH + read_bytes)) != 0)
		{
			return result;
		}

		logWarning("file: "__FILE__", line: %d, " \
			"read from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"read bytes: %d != %d", \
			__LINE__, get_binlog_readable_filename(pReader, NULL),\
			pReader->binlog_offset, read_bytes, pRecord->key.length);
		return ENOENT;
	}

	if (read(pReader->binlog_fd, buff, 1) != 1) //skip space
	{
		if ((result=rewind_to_prev_rec_end(pReader, \
			BINLOG_FIX_FIELDS_LENGTH + pRecord->key.length)) != 0)
		{
			return result;
		}

		logWarning("file: "__FILE__", line: %d, " \
			"read from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"read bytes: %d != 1", \
			__LINE__, get_binlog_readable_filename(pReader, NULL),\
			pReader->binlog_offset, read_bytes);
		return ENOENT;
	}

	if (pRecord->value.length > pRecord->value.size)
	{
		pRecord->value.size = pRecord->value.length + 1024;
		pRecord->value.data = (char *)realloc(pRecord->value.data, \
						pRecord->value.size);
		if (pRecord->value.data == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"realloc %d bytes fail, " \
				"errno: %d, error info: %s", \
				pRecord->value.size, errno, strerror(errno));
			return errno != 0 ? errno : ENOMEM;
		}
	}

	read_bytes = read(pReader->binlog_fd, pRecord->value.data, \
			pRecord->value.length);
	if (read_bytes < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"read from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"errno: %d, error info: %s", __LINE__, \
			get_binlog_readable_filename(pReader, NULL), \
			pReader->binlog_offset, errno, strerror(errno));
		return errno != 0 ? errno : EIO;
	}
	if (read_bytes != pRecord->value.length)
	{
		if ((result=rewind_to_prev_rec_end(pReader, \
			BINLOG_FIX_FIELDS_LENGTH + pRecord->key.length + 1 + \
			read_bytes)) != 0)
		{
			return result;
		}

		logWarning("file: "__FILE__", line: %d, " \
			"read from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"read bytes: %d != %d", \
			__LINE__, get_binlog_readable_filename(pReader, NULL),\
			pReader->binlog_offset, read_bytes, pRecord->value.length);
		return ENOENT;
	}

	if (read(pReader->binlog_fd, buff, 1) != 1) //skip \n
	{
		if ((result=rewind_to_prev_rec_end(pReader, \
			BINLOG_FIX_FIELDS_LENGTH + pRecord->key.length + 1 + \
			pRecord->value.length)) != 0)
		{
			return result;
		}

		logWarning("file: "__FILE__", line: %d, " \
			"read from binlog file \"%s\" fail, " \
			"file offset: "INT64_PRINTF_FORMAT", " \
			"read bytes: %d != 1", \
			__LINE__, get_binlog_readable_filename(pReader, NULL),\
			pReader->binlog_offset, read_bytes);
		return ENOENT;
	}

	*record_length = CALC_RECORD_LENGTH(pRecord->key.length, \
					pRecord->value.length);

	/*
	//printf("timestamp=%d, op_type=%c, key len=%d, value len=%d, " \
		"record length=%d, offset=%d\n", \
		(int)pRecord->timestamp, pRecord->op_type, \
		pRecord->key.length, pRecord->value.length, \
		*record_length, (int)pReader->binlog_offset);
	*/

	return 0;
}

static int fdht_binlog_reader_skip(BinLogReader *pReader)
{
	BinLogRecord record;
	int result;
	int record_len;

	memset(&record, 0, sizeof(record));
	while (1)
	{
		result = fdht_binlog_read(pReader, \
				&record, &record_len);
		if (result != 0)
		{
			if (result == ENOENT)
			{
				return 0;
			}

			return result;
		}

		if (record.timestamp >= pReader->until_timestamp)
		{
			result = rewind_to_prev_rec_end( \
					pReader, record_len);
			break;
		}

		pReader->binlog_offset += record_len;
	}

	return result;
}

static void* fdht_sync_thread_entrance(void* arg)
{
	FDHTGroupServer *pDestServer;
	BinLogReader reader;
	BinLogRecord record;
	FDHTServerInfo fdht_server;
	char local_ip_addr[IP_ADDRESS_SIZE];
	int read_result;
	int sync_result;
	int conn_result;
	int result;
	int record_len;
	int previousCode;
	int nContinuousFail;
	time_t last_active_time;
	time_t last_check_sync_cache_time;
	
	pDestServer = (FDHTGroupServer *)arg;

	memset(local_ip_addr, 0, sizeof(local_ip_addr));
	memset(&reader, 0, sizeof(reader));
	memset(&record, 0, sizeof(record));

	last_check_sync_cache_time = time(NULL);

	strcpy(fdht_server.ip_addr, pDestServer->ip_addr);
	fdht_server.port = pDestServer->port;
	fdht_server.sock = -1;
	while (g_continue_flag)
	{
		previousCode = 0;
		nContinuousFail = 0;
		conn_result = 0;
		while (g_continue_flag)
		{
			fdht_server.sock = \
				socket(AF_INET, SOCK_STREAM, 0);
			if(fdht_server.sock < 0)
			{
				logCrit("file: "__FILE__", line: %d," \
					" socket create fail, " \
					"errno: %d, error info: %s. " \
					"program exit!", __LINE__, \
					errno, strerror(errno));
				fdht_terminate();
				break;
			}

			if ((conn_result=connectserverbyip(fdht_server.sock,\
				fdht_server.ip_addr, fdht_server.port)) == 0)
			{
				char szFailPrompt[36];
				if (nContinuousFail == 0)
				{
					*szFailPrompt = '\0';
				}
				else
				{
					sprintf(szFailPrompt, \
						", continuous fail count: %d", \
						nContinuousFail);
				}
				logInfo("file: "__FILE__", line: %d, " \
					"successfully connect to " \
					"storage server %s:%d%s", __LINE__, \
					fdht_server.ip_addr, \
					fdht_server.port, szFailPrompt);
				nContinuousFail = 0;
				break;
			}

			if (previousCode != conn_result)
			{
				logError("file: "__FILE__", line: %d, " \
					"connect to storage server %s:%d fail" \
					", errno: %d, error info: %s", \
					__LINE__, \
					fdht_server.ip_addr, fdht_server.port, \
					conn_result, strerror(conn_result));
				previousCode = conn_result;
			}

			nContinuousFail++;
			close(fdht_server.sock);
			fdht_server.sock = -1;
			sleep(1);
		}

		if (nContinuousFail > 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"connect to storage server %s:%d fail, " \
				"try count: %d, errno: %d, error info: %s", \
				__LINE__, fdht_server.ip_addr, \
				fdht_server.port, nContinuousFail, \
				conn_result, strerror(conn_result));
		}

		if (!g_continue_flag)
		{
			break;
		}

		getSockIpaddr(fdht_server.sock, \
			local_ip_addr, IP_ADDRESS_SIZE);

		/*
		//printf("file: "__FILE__", line: %d, " \
			"fdht_server.ip_addr=%s, " \
			"local_ip_addr: %s\n", \
			__LINE__, fdht_server.ip_addr, local_ip_addr);
		*/

		if (strcmp(local_ip_addr, fdht_server.ip_addr) == 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"ip_addr %s belong to the local host," \
				" sync thread exit.", \
				__LINE__, fdht_server.ip_addr);
			fdht_quit(&fdht_server);
			break;
		}

		if (fdht_reader_init(&fdht_server, &reader) != 0)
		{
			if (!g_continue_flag)
			{
				break;
			}

			close(fdht_server.sock);
			fdht_server.sock = -1;
			fdht_reader_destroy(&reader);
			continue;
		}

		last_active_time = time(NULL);
		sync_result = 0;
		while (g_continue_flag)
		{
			read_result = fdht_binlog_read(&reader, \
					&record, &record_len);
			if (read_result == ENOENT)
			{
				if (reader.need_sync_old && \
					!reader.sync_old_done)
				{

				if ((result=fdht_report_sync_done(
					&fdht_server)) == 0)
				{
				reader.sync_old_done = true;
				if (fdht_write_to_mark_file(&reader) != 0)
				{
					logCrit("file: "__FILE__", line: %d, " \
						"fdht_write_to_mark_file " \
						"fail, program exit!", \
						__LINE__);
					fdht_terminate();
					break;
				}
				}

				}
				
				if (time(NULL) - last_active_time >= \
					g_heart_beat_interval)
				{
					if ((result=fdht_client_heart_beat( \
							&fdht_server)) != 0)
					{
						break;
					}

					last_active_time = time(NULL);
				}

				if ((pbinlog_write_cache_current - \
						binlog_write_cache_buff) > 0&&\
					time(NULL)-last_check_sync_cache_time \
						 >= 60)
				{
					last_check_sync_cache_time = time(NULL);
					fdht_binlog_fsync(true);
				}

				usleep(g_sync_wait_usec);
				continue;
			}
			else if (read_result != 0)
			{
				break;
			}

			if ((sync_result=fdht_sync_data(&reader, \
				&fdht_server, &record)) != 0)
			{
				if (rewind_to_prev_rec_end( \
					&reader, record_len) != 0)
				{
					logCrit("file: "__FILE__", line: %d, " \
						"rewind_to_prev_rec_end fail, "\
						"program exit!", __LINE__);
					fdht_terminate();
				}

				break;
			}

			last_active_time = time(NULL);

			++reader.scan_row_count;
			reader.binlog_offset += record_len;
			if (reader.sync_row_count % 1000 == 0)
			{
				if (fdht_write_to_mark_file(&reader) != 0)
				{
					logCrit("file: "__FILE__", line: %d, " \
						"fdht_write_to_mark_file " \
						"fail, program exit!", \
						__LINE__);
					fdht_terminate();
					break;
				}
			}
		}

		if (reader.last_write_row_count != reader.scan_row_count)
		{
			if (fdht_write_to_mark_file(&reader) != 0)
			{
				logCrit("file: "__FILE__", line: %d, " \
					"fdht_write_to_mark_file fail, " \
					"program exit!", __LINE__);
				fdht_terminate();
				break;
			}
		}

		close(fdht_server.sock);
		fdht_server.sock = -1;
		fdht_reader_destroy(&reader);

		if (!g_continue_flag)
		{
			break;
		}

		if (!(sync_result == ENOTCONN || sync_result == EIO))
		{
			sleep(1);
		}
	}

	if (fdht_server.sock >= 0)
	{
		close(fdht_server.sock);
	}
	fdht_reader_destroy(&reader);

	if ((result=pthread_mutex_lock(&sync_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_lock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
	}
	g_fdht_sync_thread_count--;
	if ((result=pthread_mutex_unlock(&sync_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_unlock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
	}

	return NULL;
}

static int fdht_sync_thread_start(const FDHTGroupServer *pDestServer)
{
	int result;
	pthread_attr_t pattr;
	pthread_t tid;

	if (is_local_host_ip(pDestServer->ip_addr)) //can't self sync to self
	{
		return 0;
	}

	if ((result=init_pthread_attr(&pattr)) != 0)
	{
		return result;
	}

	/*
	//printf("start storage ip_addr: %s:%d, g_fdht_sync_thread_count=%d\n", 
			pDestServer->ip_addr, pDestServer->port, g_fdht_sync_thread_count);
	*/

	if ((result=pthread_create(&tid, &pattr, fdht_sync_thread_entrance, \
		(void *)pDestServer)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"create thread failed, errno: %d, " \
			"error info: %s", \
			__LINE__, result, strerror(result));

		pthread_attr_destroy(&pattr);
		return result;
	}

	if ((result=pthread_mutex_lock(&sync_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_lock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
	}

	g_fdht_sync_thread_count++;
	sync_tids = (pthread_t *)realloc(sync_tids, sizeof(pthread_t) * \
					g_fdht_sync_thread_count);
	if (sync_tids == NULL)
	{
		logError("file: "__FILE__", line: %d, " \
			"malloc %d bytes fail, " \
			"errno: %d, error info: %s", \
			__LINE__, sizeof(pthread_t) * \
			g_fdht_sync_thread_count, \
			errno, strerror(errno));
	}
	else
	{
		sync_tids[g_fdht_sync_thread_count - 1] = tid;
	}

	if ((result=pthread_mutex_unlock(&sync_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_unlock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
	}

	pthread_attr_destroy(&pattr);

	return 0;
}

int write_to_sync_ini_file()
{
	char full_filename[MAX_PATH_SIZE];
	char buff[256];
	int fd;
	int len;

	snprintf(full_filename, sizeof(full_filename), \
		"%s/data/%s", g_base_path, DATA_DIR_INITED_FILENAME);
	if ((fd=open(full_filename, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"open file \"%s\" fail, " \
			"errno: %d, error info: %s", \
			__LINE__, full_filename, \
			errno, strerror(errno));
		return errno != 0 ? errno : ENOENT;
	}

	len = sprintf(buff, "%s=%d\n" \
		"%s=%d\n"  \
		"%s=%s\n"  \
		"%s=%d\n"  \
		"%s=%d\n"  \
		"%s=%d\n", \
		INIT_ITEM_SERVER_JOIN_TIME, g_server_join_time, \
		INIT_ITEM_SYNC_OLD_DONE, g_sync_old_done, \
		INIT_ITEM_SYNC_SRC_SERVER, g_sync_src_ip_addr, \
		INIT_ITEM_SYNC_SRC_PORT, g_sync_src_port, \
		INIT_ITEM_SYNC_UNTIL_TIMESTAMP, g_sync_until_timestamp, \
		INIT_ITEM_SYNC_DONE_TIMESTAMP, g_sync_done_timestamp \
	    );
	if (write(fd, buff, len) != len)
	{
		logError("file: "__FILE__", line: %d, " \
			"write to file \"%s\" fail, " \
			"errno: %d, error info: %s", \
			__LINE__, full_filename, \
			errno, strerror(errno));
		close(fd);
		return errno != 0 ? errno : EIO;
	}

	close(fd);
	return 0;
}
