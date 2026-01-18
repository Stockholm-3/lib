/**
 * @file cache.c
 * @brief Simple in-memory key-value cache with TTL-based expiration.
 *
 * This module implements a lightweight cache that stores arbitrary binary
 * data associated with string keys. Each cache entry has a time-to-live (TTL),
 * after which it is considered expired and removed on access.
 *
 * The cache uses a linked list internally and enforces a maximum number of
 * entries. When the cache is full, the oldest entry is evicted.
 *
 * Key features:
 * - String-keyed cache entries
 * - Per-entry or default TTL expiration
 * - Automatic eviction of expired entries
 * - Oldest-entry eviction when capacity is reached
 * - Deep copies of stored and retrieved data
 *
 * @note This cache is not thread-safe.
 * @note Retrieved data must be freed by the caller.
 */

#include "cache.h"

#include <stdio.h>

/**
 * @brief Free all memory associated with a cache entry.
 *
 * This helper function releases the key string, data buffer,
 * and the CacheEntry structure itself.
 *
 * @param entry Pointer to the CacheEntry to free.
 */
static void free_cache_entry(CacheEntry* entry) {
    if (entry) {
        free(entry->key);
        free(entry->data);
        free(entry);
    }
}

/**
 * @brief Check whether a cache entry has expired.
 *
 * An entry is considered expired if the current system time
 * is greater than the entry's expiry timestamp.
 *
 * @param entry Pointer to the CacheEntry to check.
 *
 * @return
 *   - 1 if the entry is NULL or expired.
 *   - 0 if the entry is still valid.
 */
static int is_expired(CacheEntry* entry) {
    if (!entry) {
        return 1;
    }
    return (time(NULL) > entry->expiry);
}

/**
 * @brief Create and initialize a cache instance.
 *
 * This function allocates a Cache structure and initializes
 * its internal linked list and configuration values.
 *
 * @param max_size     Maximum number of entries allowed in the cache.
 * @param default_ttl  Default time-to-live (in seconds) for entries
 *                     when no TTL is explicitly provided.
 *
 * @return
 *   - Pointer to a newly created Cache on success.
 *   - NULL on allocation failure.
 */
Cache* cache_create(size_t max_size, time_t default_ttl) {
    Cache* cache = (Cache*)malloc(sizeof(Cache));
    if (!cache) {
        return NULL;
    }

    cache->entries = linked_list_create();
    if (!cache->entries) {
        free(cache);
        return NULL;
    }

    cache->max_size    = max_size;
    cache->default_ttl = default_ttl;
    return cache;
}

/**
 * @brief Destroy a cache and free all associated resources.
 *
 * This function removes all cache entries, frees internal data
 * structures, and releases the Cache object itself.
 *
 * @param cache Pointer to the Cache to destroy.
 */
void cache_destroy(Cache* cache) {
    if (!cache) {
        return;
    }
    linked_list_clear(cache->entries, (void (*)(void*))free_cache_entry);
    linked_list_dispose(&cache->entries, NULL);
    free(cache);
}

/**
 * @brief Insert or update a cache entry.
 *
 * If an entry with the same key already exists, it is removed
 * before inserting the new value.
 *
 * If the cache has reached its maximum size, the oldest entry
 * is evicted to make room for the new one.
 *
 * The provided data is copied internally and does not need
 * to remain valid after this call.
 *
 * @param cache     Pointer to the Cache.
 * @param key       Null-terminated string key.
 * @param data      Pointer to the data to store.
 * @param data_size Size of the data in bytes.
 * @param ttl       Time-to-live in seconds. If <= 0, the cache's
 *                  default TTL is used.
 *
 * @return
 *   - 0 on success.
 *   - -1 on invalid arguments or allocation failure.
 */
int cache_set(Cache* cache, const char* key, void* data, size_t data_size,
              time_t ttl) {
    if (!cache || !key || !data) {
        return -1;
    }

    // Remove existing entry if it exists
    cache_remove(cache, key);

    // Evict oldest entry if cache is full
    if (cache->entries->size >= cache->max_size) {
        if (cache->entries->head) {
            CacheEntry* oldest =
                (CacheEntry*)cache->entries->head->item;
            cache_remove(cache, oldest->key);
        }
    }

    // Allocate new entry
    CacheEntry* entry = (CacheEntry*)malloc(sizeof(CacheEntry));
    if (!entry) {
        return -1;
    }

    entry->key  = strdup(key);
    entry->data = malloc(data_size);
    if (!entry->key || !entry->data) {
        free_cache_entry(entry);
        return -1;
    }

    memcpy(entry->data, data, data_size);
    entry->data_size = data_size;
    entry->timestamp = time(NULL);
    entry->expiry    =
        entry->timestamp + (ttl > 0 ? ttl : cache->default_ttl);

    // Add entry to cache
    if (linked_list_append(cache->entries, entry) != 0) {
        free_cache_entry(entry);
        return -1;
    }

    return 0;
}

/**
 * @brief Retrieve a value from the cache.
 *
 * If the entry exists but has expired, it is removed and
 * NULL is returned.
 *
 * The returned data is a newly allocated copy and must
 * be freed by the caller.
 *
 * @param cache     Pointer to the Cache.
 * @param key       Null-terminated string key.
 * @param data_size Optional output parameter that receives
 *                  the size of the returned data.
 *
 * @return
 *   - Pointer to a newly allocated data copy on success.
 *   - NULL if the key is not found or the entry is expired.
 */
void* cache_get(Cache* cache, const char* key, size_t* data_size) {
    if (!cache || !key) {
        return NULL;
    }

    LinkedList_foreach(cache->entries, node) {
        CacheEntry* entry = (CacheEntry*)node->item;
        if (strcmp(entry->key, key) == 0) {
            if (is_expired(entry)) {
                cache_remove(cache, key);
                return NULL;
            }

            if (data_size) {
                *data_size = entry->data_size;
            }

            void* data_copy = malloc(entry->data_size);
            if (data_copy) {
                memcpy(data_copy, entry->data, entry->data_size);
            }
            return data_copy;
        }
    }

    return NULL;
}

/**
 * @brief Remove an entry from the cache by key.
 *
 * If the key does not exist, the function returns silently.
 *
 * @param cache Pointer to the Cache.
 * @param key   Null-terminated string key to remove.
 */
void cache_remove(Cache* cache, const char* key) {
    if (!cache || !key) {
        return;
    }

    size_t index = 0;
    LinkedList_foreach(cache->entries, node) {
        CacheEntry* entry = (CacheEntry*)node->item;
        if (strcmp(entry->key, key) == 0) {
            linked_list_pop(cache->entries, index,
                            (void (*)(void*))free_cache_entry);
            return;
        }
        index++;
    }
}

/**
 * @brief Remove all entries from the cache.
 *
 * This function clears the cache but does not free
 * the Cache structure itself.
 *
 * @param cache Pointer to the Cache.
 */
void cache_clear(Cache* cache) {
    if (!cache) {
        return;
    }
    linked_list_clear(cache->entries, (void (*)(void*))free_cache_entry);
}