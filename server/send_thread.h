/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/

//send_thread.h

#ifndef _SEND_THREAD_H
#define _SEND_THREAD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <event.h>
#include "fdht_define.h"
#include "task_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

int send_notify_write();

int send_thread_init();
int send_process_init();
int send_set_event(struct task_info *pTask, int sock);
int send_add_event(struct task_info *pTask);

#ifdef __cplusplus
}
#endif

#endif

