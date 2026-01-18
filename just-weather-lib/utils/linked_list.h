/**
 * @file linked_list.h
 * @brief Doubly linked list container for generic pointer storage.
 *
 * This module provides a simple doubly linked list implementation
 * for storing generic pointers (`void*`).
 *
 * The list supports indexed access, insertion, removal, and
 * iteration using a convenience macro.
 *
 * Memory ownership of stored items is controlled by the caller.
 * Optional free callbacks can be supplied to properly dispose
 * stored elements when nodes are removed.
 *
 * Key features:
 * - Doubly linked nodes
 * - Indexed access and insertion
 * - Append and removal operations
 * - Safe iteration via macro
 * - Optional item cleanup via callback
 *
 * @note This module is not thread-safe.
 * @note The list stores raw pointers and does not manage item
 *       lifetime unless a free callback is provided.
 */

#ifndef LINKED_LIST_H
#define LINKED_LIST_H

#include <stddef.h>

/**
 * @brief Linked list node structure.
 *
 * Each node stores a pointer to an item and links
 * to the previous and next nodes in the list.
 */
typedef struct Node Node;
struct Node {
    Node* back;  /**< Pointer to the previous node */
    Node* front; /**< Pointer to the next node */
    void* item;  /**< Stored item pointer */
};

/**
 * @brief Doubly linked list container.
 */
typedef struct {
    Node*  head; /**< First node in the list */
    Node*  tail; /**< Last node in the list */
    size_t size; /**< Number of nodes in the list */
} LinkedList;

/**
 * @brief Iterate over all nodes in a linked list.
 *
 * This macro allows safe forward traversal of the list:
 *
 * @code
 * LinkedList_foreach(list, node) {
 *     MyType* item = node->item;
 * }
 * @endcode
 *
 * @param list Pointer to the LinkedList.
 * @param node Name of the Node* iteration variable.
 */
#define LinkedList_foreach(list, node)                                         \
    for (Node * (node) = (list)->head; (node) != NULL; (node) = (node)->front)

/**
 * @brief Create and initialize an empty linked list.
 *
 * @return
 *   - Pointer to a newly allocated LinkedList on success.
 *   - NULL on allocation failure.
 */
LinkedList* linked_list_create();

/**
 * @brief Retrieve a node at a specific index.
 *
 * Indexing starts at 0.
 *
 * @param list  Pointer to the LinkedList.
 * @param index Zero-based index of the node.
 *
 * @return
 *   - Pointer to the Node at the given index.
 *   - NULL if the index is out of range.
 */
Node* linked_list_get_index(LinkedList* list, size_t index);

/**
 * @brief Insert an item into the list at a specific index.
 *
 * Existing items at and after the index are shifted
 * one position forward.
 *
 * @param list  Pointer to the LinkedList.
 * @param index Zero-based index at which to insert.
 * @param item  Pointer to the item to store.
 *
 * @return
 *   - 0 on success.
 *   - -1 on invalid arguments or allocation failure.
 */
int linked_list_insert(LinkedList* list, size_t index, void* item);

/**
 * @brief Append an item to the end of the list.
 *
 * @param list Pointer to the LinkedList.
 * @param item Pointer to the item to store.
 *
 * @return
 *   - 0 on success.
 *   - -1 on failure.
 */
int linked_list_append(LinkedList* list, void* item);

/**
 * @brief Remove a node from the list by reference.
 *
 * The node is freed when removed and must not be accessed again.
 * The stored item is only freed if a free callback is provided.
 *
 * @param list          Pointer to the LinkedList.
 * @param item          Pointer to the Node to remove.
 * @param free_function Optional callback used to free the stored item.
 *
 * @return
 *   - 0 on success.
 *   - -1 on failure.
 *
 * @note For basic heap-allocated items, the standard free() function
 *       can be used as the free callback.
 */
int linked_list_remove(LinkedList* list, Node* item,
                       void (*free_function)(void*));

/**
 * @brief Remove a node from the list by index.
 *
 * Behaves the same as linked_list_remove(), but locates
 * the node using a zero-based index.
 *
 * @param list          Pointer to the LinkedList.
 * @param index         Zero-based index of the node to remove.
 * @param free_function Optional callback used to free the stored item.
 *
 * @return
 *   - 0 on success.
 *   - -1 on failure.
 */
int linked_list_pop(LinkedList* list, size_t index,
                    void (*free_function)(void*));

/**
 * @brief Remove all nodes from the list.
 *
 * All nodes are freed. Stored items are freed only if a
 * free callback is provided.
 *
 * @param list          Pointer to the LinkedList.
 * @param free_function Optional callback used to free stored items.
 *
 * @note For basic heap-allocated items, the standard free()
 *       function can be used.
 */
void linked_list_clear(LinkedList* list, void (*free_function)(void*));

/**
 * @brief Dispose of an entire linked list.
 *
 * All nodes are removed and the list structure itself is freed.
 * The caller's pointer is set to NULL to prevent dangling references.
 *
 * @param list          Pointer to a LinkedList pointer.
 * @param free_function Optional callback used to free stored items.
 *
 * @note For basic heap-allocated items, the standard free()
 *       function can be used.
 */
void linked_list_dispose(LinkedList** list, void (*free_function)(void*));

#endif /* LINKED_LIST_H */
