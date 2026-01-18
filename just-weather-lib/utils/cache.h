/**
 * @file cache.h
 * @brief In-memory key-value cache with TTL-based expiration.
 *
 * This module provides a simple cache implementation for storing
 * arbitrary binary data associated with string keys.
 *
 * Each cache entry has an expiration time (TTL). Expired entries
 * are automatically removed when accessed.
 *
 * The cache enforces a maximum number of entries. When the cache
 * reaches capacity, the oldest entry is evicted to make room
 * for new entries.
 *
 * Key features:
 * - String-keyed entries
 * - Configurable default TTL
 * - Per-entry TTL override
 * - Automatic expiration handling
 * - Oldest-entry eviction policy
 *
 * @note This cache is not thread-safe.
 * @note Data returned by cache_get() must be freed by the caller.
 */

#ifndef CACHE_H
#define CACHE_H

#include "linked_list.h"

#include <time.h>

/**
 * @brief Represents a single cache entry.
 *
 * A cache entry stores a deep copy of user-provided data along
 * with metadata used for expiration and eviction.
 */
typedef struct {
    char*  key;       /**< Cache key (null-terminated string) */
    void*  data;      /**< Cached data buffer */
    size_t data_size; /**< Size of cached data in bytes */
    time_t timestamp; /**< Time when the entry was created */
    time_t expiry;    /**< Absolute expiration time */
} CacheEntry;

/**
 * @brief Cache container structure.
 *
 * The cache stores entries internally using a linked list and
 * enforces a maximum number of entries.
 */
typedef struct {
    LinkedList* entries;     /**< List of CacheEntry objects */
    size_t      max_size;    /**< Maximum number of cache entries */
    time_t      default_ttl; /**< Default time-to-live in seconds */
} Cache;

/**
 * @brief Create and initialize a cache instance.
 *
 * Allocates a Cache structure and initializes its internal
 * storage and configuration.
 *
 * @param max_size     Maximum number of entries allowed.
 * @param default_ttl  Default time-to-live (in seconds) for entries
 *                     when no explicit TTL is provided.
 *
 * @return
 *   - Pointer to a newly created Cache on success.
 *   - NULL on allocation failure.
 */
Cache* cache_create(size_t max_size, time_t default_ttl);

/**
 * @brief Destroy a cache and free all associated resources.
 *
 * Removes all entries from the cache, releases internal data
 * structures, and frees the Cache object itself.
 *
 * @param cache Pointer to the Cache to destroy.
 */
void cache_destroy(Cache* cache);

/**
 * @brief Insert or update a cache entry.
 *
 * If an entry with the same key already exists, it is replaced.
 * If the cache is full, the oldest entry is evicted.
 *
 * The data is copied internally and does not need to remain valid
 * after this call.
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
              time_t ttl);

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
 *   - NULL if the key is not found or the entry has expired.
 */
void* cache_get(Cache* cache, const char* key, size_t* data_size);

/**
 * @brief Remove a cache entry by key.
 *
 * If the key does not exist, this function returns silently.
 *
 * @param cache Pointer to the Cache.
 * @param key   Null-terminated string key to remove.
 */
void cache_remove(Cache* cache, const char* key);

/**
 * @brief Remove all entries from the cache.
 *
 * Clears the cache contents without freeing the Cache
 * structure itself.
 *
 * @param cache Pointer to the Cache.
 */
void cache_clear(Cache* cache);

#endif /* CACHE_H */
