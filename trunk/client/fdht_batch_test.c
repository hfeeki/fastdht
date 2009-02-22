/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include "fdfs_base64.h"
#include "fdht_global.h"
#include "sockopt.h"
#include "logger.h"
#include "shared_func.h"
#include "fdht_types.h"
#include "fdht_proto.h"
#include "fdht_client.h"
#include "fdht_func.h"

int main(int argc, char *argv[])
{
	char *conf_filename;
	int result;
	int expires;
	FDHTObjectInfo object_info;
	FDHTKeyValuePair key_list[32];
	int key_count;
	int success_count;
	int i;
	int conn_success_count;
	int conn_fail_count;

	printf("This is FastDHT client test program v%d.%d\n" \
"\nCopyright (C) 2008, Happy Fish / YuQing\n" \
"\nFastDHT may be copied only under the terms of the GNU General\n" \
"Public License V3, which may be found in the FastDHT source kit.\n" \
"Please visit the FastDHT Home Page http://www.csource.org/ \n" \
"for more detail.\n\n" \
, g_version.major, g_version.minor);

	if (argc < 2)
	{
		printf("Usage: %s <config_file>\n", argv[0]);
		return 1;
	}

	conf_filename = argv[1];

	g_log_level = LOG_DEBUG;
	if ((result=fdht_client_init(conf_filename)) != 0)
	{
		return result;
	}

	//g_keep_alive = true;
	if (g_keep_alive)
	{
		if ((result=fdht_connect_all_servers(&g_group_array, true, \
			&conn_success_count, &conn_fail_count)) != 0)
		{
			printf("fdht_connect_all_servers fail, " \
				"error code: %d, error info: %s\n", \
				result, strerror(result));
			return result;
		}
	}

	srand(time(NULL));

	expires = time(NULL) + 3600;
	memset(&object_info, 0, sizeof(object_info));
	object_info.namespace_len = sprintf(object_info.szNameSpace, "user");
	object_info.obj_id_len = sprintf(object_info.szObjectId, "happy_fish");


	memset(key_list, 0, sizeof(key_list));
	key_count = 4;
	key_list[0].key_len = sprintf(key_list[0].szKey, "login");
	key_list[1].key_len = sprintf(key_list[1].szKey, "reg");
	key_list[2].key_len = sprintf(key_list[2].szKey, "intl");
	key_list[3].key_len = sprintf(key_list[3].szKey, "co");
	while (1)
	{
		key_list[0].pValue = "happy_fish";
		key_list[0].value_len = strlen(key_list[0].pValue);
		key_list[1].pValue = "1235277184";
		key_list[1].value_len = strlen(key_list[1].pValue);
		key_list[2].pValue = "zh";
		key_list[2].value_len = strlen(key_list[2].pValue);
		key_list[3].pValue = "cn";
		key_list[3].value_len = strlen(key_list[3].pValue);

		if ((result=fdht_batch_set(&object_info, key_list, \
				key_count, expires, &success_count)) != 0)
		{
			printf("fdht_batch_set result=%d\n", result);
			break;
		}
		printf("fdht_batch_set success count: %d\n", success_count);

		for (i=0; i<key_count; i++)
		{
			key_list[i].pValue = NULL;
			key_list[i].value_len = 0;
		}
		if ((result=fdht_batch_get_ex(&object_info, key_list, \
				key_count, expires, &success_count)) != 0)
		{
			printf("fdht_batch_get_ex result=%d\n", result);
			break;
		}
		printf("fdht_batch_get_ex success count: %d\n", success_count);

		for (i=0; i<key_count; i++)
		{
			if (key_list[i].status == 0)
			{
				printf("key=%s, value=%s(%d)\n", \
					key_list[i].szKey, key_list[i].pValue, \
					key_list[i].value_len);
			}
			else
			{
				printf("key=%s, status=%d\n", \
					key_list[i].szKey, key_list[i].status);
			}
		}

		for (i=0; i<key_count; i++)
		{
			if (key_list[i].pValue != NULL)
			{
				free(key_list[i].pValue);
			}
		}

		if ((result=fdht_batch_delete(&object_info, key_list, \
				key_count, &success_count)) != 0)
		{
			printf("fdht_batch_delete result=%d\n", result);
			break;
		}

		printf("fdht_batch_delete success count: %d\n", success_count);

		break;
	}

	if (g_keep_alive)
	{
		fdht_disconnect_all_servers(&g_group_array);
	}
	
	fdht_client_destroy();

	return result;
}

