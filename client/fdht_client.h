/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/

//fdht_client.h

#ifndef _FDHT_CLIENT_H
#define _FDHT_CLIENT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fdht_define.h"
#include "fdht_types.h"
#include "fdht_proto.h"
#include "fdht_func.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* (*MallocFunc)(size_t size);

extern GroupArray g_group_array; //group info, including server list
extern bool g_keep_alive;  //persistent connection flag

/*
init function
param:
	filename: client config filename
return: 0 for success, != 0 for fail (errno)
*/
int fdht_client_init(const char *filename);


/*
destroy function, free resource
param:
return: none
*/
void fdht_client_destroy();

#define fdht_get(pKeyInfo, ppValue, value_len) \
	fdht_get_ex1((&g_group_array), g_keep_alive, pKeyInfo, \
		FDHT_EXPIRES_NONE, ppValue, value_len, malloc)

#define fdht_get_ex(pKeyInfo, expires, ppValue, value_len) \
	fdht_get_ex1((&g_group_array), g_keep_alive, pKeyInfo, expires, \
			ppValue, value_len, malloc)

#define  fdht_batch_get(pObjectInfo, key_list, key_count) \
	fdht_batch_get_ex1((&g_group_array), g_keep_alive, pObjectInfo, \
			key_list, key_count, FDHT_EXPIRES_NONE, malloc)

#define  fdht_batch_get_ex(pObjectInfo, key_list, key_count, expires) \
	fdht_batch_get_ex1((&g_group_array), g_keep_alive, pObjectInfo, \
			key_list, key_count, expires, malloc)

#define fdht_set(pKeyInfo, expires, pValue, value_len) \
	fdht_set_ex((&g_group_array), g_keep_alive, pKeyInfo, expires, \
		pValue, value_len)

#define fdht_inc(pKeyInfo, expires, increase, pValue, value_len) \
	fdht_inc_ex((&g_group_array), g_keep_alive, pKeyInfo, expires, \
		increase, pValue, value_len)

#define fdht_delete(pKeyInfo) \
	fdht_delete_ex((&g_group_array), g_keep_alive, pKeyInfo)


/*
get value of the key
param:
	pGroupArray: group info, can use &g_group_array
	bKeepAlive: persistent connection flag, true for persistent connection
	pKeyInfo:  the key to fetch
	expires:  expire time (unix timestamp)
		FDHT_EXPIRES_NONE - do not change the expire time of the key
		FDHT_EXPIRES_NEVER- set the expire time to forever(never expired)
	ppValue: return the value of the key
	value_len: return the length of the value (bytes)
	malloc_func: malloc function, can be standard function named malloc
return: 0 for success, != 0 for fail (errno)
*/
int fdht_get_ex1(GroupArray *pGroupArray, const bool bKeepAlive, \
		FDHTKeyInfo *pKeyInfo, const time_t expires, \
		char **ppValue, int *value_len, MallocFunc malloc_func);

/*
get values of the key list
param:
	pGroupArray: group info, can use &g_group_array
	bKeepAlive: persistent connection flag, true for persistent connection
	pObjectInfo:  the object to fetch, namespace and object id can't be empty
	key_list: key list, return the value of the key
	key_count: key count
	expires:  expire time (unix timestamp)
		FDHT_EXPIRES_NONE - do not change the expire time of the keys
		FDHT_EXPIRES_NEVER- set the expire time to forever(never expired)
	malloc_func: malloc function, can be standard function named malloc
return: 0 for success, != 0 for fail (errno)
*/
int fdht_batch_get_ex1(GroupArray *pGroupArray, const bool bKeepAlive, \
		FDHTObjectInfo *pObjectInfo, FDHTKeyValuePair *key_list, \
		const int key_count, const time_t expires, \
		MallocFunc malloc_func);

/*
set value of the key
param:
	pGroupArray: group info, can use &g_group_array
	bKeepAlive: persistent connection flag, true for persistent connection
	pKeyInfo:  the key to set
	expires:  expire time (unix timestamp)
		FDHT_EXPIRES_NEVER- set the expire time to forever(never expired)
	pValue: the value of the key
	value_len: the length of the value (bytes)
return: 0 for success, != 0 for fail (errno)
*/
int fdht_set_ex(GroupArray *pGroupArray, const bool bKeepAlive, \
		FDHTKeyInfo *pKeyInfo, const time_t expires, \
		const char *pValue, const int value_len);

/*
increase value of the key, if the key does not exist, 
set the value to increment value (param named "increase")
param:
	pGroupArray: group info, can use &g_group_array
	bKeepAlive: persistent connection flag, true for persistent connection
	pKeyInfo:  the key to increase 
	expires:  expire time (unix timestamp)
		FDHT_EXPIRES_NEVER- set the expire time to forever(never expired)
	increase: the increment value, can be negative, eg. 1 or -1
	pValue: return the value after increment
	value_len: return the length of the value (bytes)
return: 0 for success, != 0 for fail (errno)
*/
int fdht_inc_ex(GroupArray *pGroupArray, const bool bKeepAlive, \
		FDHTKeyInfo *pKeyInfo, const time_t expires, \
		const int increase, char *pValue, int *value_len);

/*
delete the key
param:
	pGroupArray: group info, can use &g_group_array
	bKeepAlive: persistent connection flag, true for persistent connection
	pKeyInfo:  the key to delete
return: 0 for success, != 0 for fail (errno)
*/
int fdht_delete_ex(GroupArray *pGroupArray, const bool bKeepAlive, \
		FDHTKeyInfo *pKeyInfo);

#ifdef __cplusplus
}
#endif

#endif

