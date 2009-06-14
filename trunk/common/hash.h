/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/

#ifndef _HASH_H_
#define _HASH_H_

#include <sys/types.h>
#include "common_define.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CRC32_XINIT 0xFFFFFFFF		/* initial value */
#define CRC32_XOROT 0xFFFFFFFF		/* final xor value */

typedef int (*HashFunc) (const void *key, const int key_len);

#ifdef HASH_MALLOC_VALUE
    #define HASH_VALUE(hash_data)  (hash_data->key + hash_data->key_len)
#else
    #define HASH_VALUE(hash_data)  hash_data->value
#endif

typedef struct tagHashData
{
	int key_len;
	int value_len;
	int malloc_value_size;
	unsigned int hash_code;
#ifndef HASH_MALLOC_VALUE
	char *value;
#endif
	char key[0];
} HashData;

typedef struct tagHashBucket
{
	HashData **items;
	int alloc_count;
	int count;
} HashBucket;

typedef struct tagHashArray
{
	HashBucket *buckets;
	HashFunc hash_func;
	int item_count;
	unsigned int *capacity;
	double load_factor;
	int64_t max_bytes;
	int64_t bytes_used;
	bool is_malloc_capacity;
} HashArray;

typedef struct tagHashStat
{
	unsigned int capacity;
	int item_count;
	int bucket_used;
	double bucket_avg_length;
	int bucket_max_length;
} HashStat;

/*
hash walk function
index: item index based 0
data: hash data, including key and value
args: passed by hash_walk function
return 0 for success, != 0 for error
*/
typedef int (*HashWalkFunc)(const int index, const HashData *data, void *args);

#define hash_init(pHash, hash_func, capacity, load_factor) \
	hash_init_ex(pHash, hash_func, capacity, load_factor, 0)

#define hash_insert(pHash, key, key_len, value) \
	hash_insert_ex(pHash, key, key_len, value, 0)

int hash_init_ex(HashArray *pHash, HashFunc hash_func, \
		const unsigned int capacity, const double load_factor, \
		const int64_t max_bytes);

void hash_destroy(HashArray *pHash);
int hash_insert_ex(HashArray *pHash, const void *key, const int key_len, \
		void *value, const int value_len);

void *hash_find(HashArray *pHash, const void *key, const int key_len);
HashData *hash_find_ex(HashArray *pHash, const void *key, const int key_len);

int hash_delete(HashArray *pHash, const void *key, const int key_len);
int hash_walk(HashArray *pHash, HashWalkFunc walkFunc, void *args);
int hash_best_op(HashArray *pHash, const int suggest_capacity);
int hash_stat(HashArray *pHash, HashStat *pStat, \
		int *stat_by_lens, const int stat_size);
void hash_stat_print(HashArray *pHash);

int RSHash(const void *key, const int key_len);

int JSHash(const void *key, const int key_len);
int JSHash_ex(const void *key, const int key_len, \
	const int init_value);

int PJWHash(const void *key, const int key_len);
int PJWHash_ex(const void *key, const int key_len, \
	const int init_value);

int ELFHash(const void *key, const int key_len);
int ELFHash_ex(const void *key, const int key_len, \
	const int init_value);

int BKDRHash(const void *key, const int key_len);
int BKDRHash_ex(const void *key, const int key_len, \
	const int init_value);

int SDBMHash(const void *key, const int key_len);
int SDBMHash_ex(const void *key, const int key_len, \
	const int init_value);

int Time33Hash(const void *key, const int key_len);
int Time33Hash_ex(const void *key, const int key_len, \
	const int init_value);

int DJBHash(const void *key, const int key_len);
int DJBHash_ex(const void *key, const int key_len, \
	const int init_value);

int APHash(const void *key, const int key_len);
int APHash_ex(const void *key, const int key_len, \
	const int init_value);

int calc_hashnr (const void* key, const int key_len);

int calc_hashnr1(const void* key, const int key_len);
int calc_hashnr1_ex(const void* key, const int key_len, \
	const int init_value);

int simple_hash(const void* key, const int key_len);
int simple_hash_ex(const void* key, const int key_len, \
	const int init_value);

int CRC32(void *key, const int key_len);
int CRC32_ex(void *key, const int key_len, \
	const int init_value);
#define CRC32_FINAL(crc)  (crc ^ CRC32_XOROT)


#define INIT_HASH_CODES4(hash_codes) \
	hash_codes[0] = CRC32_XINIT; \
	hash_codes[1] = 0; \
	hash_codes[2] = 0; \
	hash_codes[3] = 0; \


#define CALC_HASH_CODES4(buff, buff_len, hash_codes) \
	hash_codes[0] = CRC32_ex(buff, buff_len, hash_codes[0]); \
	hash_codes[1] = ELFHash_ex(buff, buff_len, hash_codes[1]); \
	hash_codes[2] = APHash_ex(buff, buff_len, hash_codes[2]); \
	hash_codes[3] = Time33Hash_ex(buff, buff_len, hash_codes[3]); \


#define FINISH_HASH_CODES4(hash_codes) \
	hash_codes[0] = CRC32_FINAL(hash_codes[0]); \


#ifdef __cplusplus
}
#endif

#endif

