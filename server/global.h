/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/

//global.h

#ifndef _GLOBAL_H
#define _GLOBAL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <event.h>
#include <pthread.h>
#include "fdht_define.h"
#include "fdht_global.h"
#include "fdht_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FDHT_MAX_LOCAL_IP_ADDRS     4

struct thread_data
{
	struct event_base *ev_base;
	int pipe_fds[2];
};

extern bool g_continue_flag;

extern int g_server_port;
extern int g_max_connections;
extern int g_max_threads;
extern int g_max_pkg_size;
extern int g_min_buff_size;
extern int g_heart_beat_interval;
extern bool g_write_to_binlog_flag;

extern int g_thread_count;
extern int g_sync_wait_usec;
extern int g_sync_log_buff_interval;
extern int g_sync_binlog_buff_interval;
extern TimeInfo g_sync_db_time_base;
extern int g_sync_db_interval;
extern TimeInfo g_clear_expired_time_base;
extern int g_clear_expired_interval;
extern int g_db_dead_lock_detect_interval;
extern TimeInfo g_compress_binlog_time_base;
extern int g_compress_binlog_interval;

extern struct timeval g_network_tv;

extern FDHTServerStat g_server_stat;

extern int g_group_count;
extern FDHTGroupServer *g_group_servers;
extern int g_group_server_count;

extern int g_server_join_time;
extern bool g_sync_old_done;
extern char g_sync_src_ip_addr[IP_ADDRESS_SIZE];
extern int g_sync_src_port;
extern int g_sync_until_timestamp;
extern int g_sync_done_timestamp;

extern int g_allow_ip_count;  /* -1 means match any ip address */
extern in_addr_t *g_allow_ip_addrs;  /* sorted array, asc order */

extern int g_local_host_ip_count;
extern char g_local_host_ip_addrs[FDHT_MAX_LOCAL_IP_ADDRS * \
				IP_ADDRESS_SIZE];

extern time_t g_server_start_time;
extern int g_store_type;
extern int g_mpool_init_capacity;
extern double g_mpool_load_factor;
extern int g_mpool_clear_min_interval;
extern struct thread_data *g_thread_data;

extern int g_thread_stack_size;

void load_local_host_ip_addrs();
bool is_local_host_ip(const char *client_ip);
int insert_into_local_host_ip(const char *client_ip);
void print_local_host_ip_addrs();

#ifdef __cplusplus
}
#endif

#endif
