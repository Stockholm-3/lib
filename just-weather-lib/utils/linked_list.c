/**
 * @file linked_list.c
 * @brief Implementation of a generic doubly linked list.
 *
 * This file implements the functions declared in linked_list.h.
 * The linked list stores generic pointers (`void*`) and allows
 * indexed access, insertion, removal, and full disposal.
 *
 * The list does not assume ownership of stored items unless
 * an explicit free callback is provided.
 *
 * @note This module is not thread-safe.
 */

#include "linked_list.h"

#include <stdio.h>
#include <stdlib.h>

/**
 * @brief Create and initialize an empty linked list.
 *
 * Allocates a zero-initialized LinkedList structure.
 *
 * @return
 *   - Pointer to a newly allocated LinkedList on success.
 *   - NULL on allocation failure.
 */
LinkedList* linked_list_create() {
    LinkedList* new_list = calloc(
        1, sizeof(LinkedList)); /* zeroed allocation, just what we need */
    if (!new_list) {
        printf("[LinkedList] Allocation error in linked_list_create\n");
        return NULL;
    }
    return new_list;
}

/**
 * @brief Retrieve a node at a specific index.
 *
 * This function performs bidirectional traversal, starting
 * from the head or tail depending on which is closer.
 *
 * @param list  Pointer to the LinkedList.
 * @param index Zero-based index of the desired node.
 *
 * @return
 *   - Pointer to the Node at the given index.
 *   - NULL if the list is NULL or the index is out of range.
 */
Node* linked_list_get_index(LinkedList* list, size_t index) {
    if (list == NULL || index >= list->size) {
        return NULL;
    }

    size_t pos = 0;
    Node*  cur = NULL;

    if (index <= list->size / 2) {
        cur = list->head;
        while (pos < index) {
            cur = cur->front;
            pos++;
        }
    } else {
        cur = list->tail;
        pos = list->size - 1;
        while (pos > index) {
            cur = cur->back;
            pos--;
        }
    }
    return cur;
}

/**
 * @brief Append an item to the end of the list.
 *
 * @param list Pointer to the LinkedList.
 * @param item Pointer to the item to store.
 *
 * @return
 *   - 0 on success.
 *   - 1 on failure.
 */
int linked_list_append(LinkedList* list, void* item) {
    if (list == NULL) {
        return 1;
    }
    Node* new_node = calloc(1, sizeof(Node));
    if (new_node == NULL) {
        return 1;
    }

    new_node->item = item;
    list->size++;

    if (list->tail == NULL) {
        list->head = new_node;
    } else {
        new_node->back    = list->tail;
        list->tail->front = new_node;
    }
    list->tail = new_node;

    return 0;
}

/**
 * @brief Insert an item into the list at a specific index.
 *
 * If the index is greater than or equal to the list size,
 * the item is appended to the end of the list.
 *
 * @param list  Pointer to the LinkedList.
 * @param index Zero-based index at which to insert.
 * @param item  Pointer to the item to store.
 *
 * @return
 *   - 0 on success.
 *   - 1 on failure.
 */
int linked_list_insert(LinkedList* list, size_t index, void* item) {
    if (list == NULL) {
        return 1;
    }
    if (index >= list->size) {
        return linked_list_append(list, item); /* append fallback */
    }

    Node* target = linked_list_get_index(list, index);
    if (target == NULL) {
        return 1;
    }

    Node* new_node = calloc(1, sizeof(Node));
    if (new_node == NULL) {
        return 1;
    }

    new_node->item = item;
    list->size++;

    new_node->back  = target->back;
    new_node->front = target;

    if (target->back != NULL) {
        target->back->front = new_node;
    } else {
        list->head = new_node;
    }
    target->back = new_node;

    return 0;
}

/**
 * @brief Remove a node from the list by reference.
 *
 * The node is freed and must not be accessed again after
 * removal. The stored item is only freed if a free callback
 * is provided.
 *
 * @param list          Pointer to the LinkedList.
 * @param item          Pointer to the Node to remove.
 * @param free_function Optional callback used to free the stored item.
 *
 * @return
 *   - 0 on success.
 *   - 1 on failure.
 */
int linked_list_remove(LinkedList* list, Node* item,
                       void (*free_function)(void*)) {
    if (list == NULL || item == NULL) {
        return 1;
    }

    Node* back  = item->back;
    Node* front = item->front;

    if (back != NULL) {
        back->front = front;
    } else {
        list->head = front;
    }

    if (front != NULL) {
        front->back = back;
    } else {
        list->tail = back;
    }

    list->size--;

    item->back  = NULL;
    item->front = NULL;

    if (free_function != NULL) {
        free_function(item->item);
    }

    free(item);

    return 0;
}

/**
 * @brief Remove a node from the list by index.
 *
 * This function locates the node at the given index and
 * removes it using linked_list_remove().
 *
 * @param list          Pointer to the LinkedList.
 * @param index         Zero-based index of the node to remove.
 * @param free_function Optional callback used to free the stored item.
 *
 * @return
 *   - 0 on success.
 *   - 1 on failure.
 */
int linked_list_pop(LinkedList* list, size_t index,
                    void (*free_function)(void*)) {
    Node* item = linked_list_get_index(list, index);
    if (item == NULL) {
        return 1;
    }
    return linked_list_remove(list, item, free_function);
}

/**
 * @brief Remove all nodes from the list.
 *
 * All nodes are freed. Stored items are freed only if a
 * free callback is provided.
 *
 * @param list          Pointer to the LinkedList.
 * @param free_function Optional callback used to free stored items.
 */
void linked_list_clear(LinkedList* list, void (*free_function)(void*)) {
    if (list == NULL) {
        return;
    }

    Node* cur = list->head;
    while (cur) {
        Node* next = cur->front;
        if (free_function != NULL) {
            free_function(cur->item);
        }
        free(cur);
        cur = next;
    }

    list->head = NULL;
    list->tail = NULL;
    list->size = 0;
}

/**
 * @brief Dispose of an entire linked list.
 *
 * Removes all nodes, frees stored items if a free callback
 * is provided, frees the list structure itself, and sets
 * the caller's pointer to NULL.
 *
 * @param list          Pointer to a LinkedList pointer.
 * @param free_function Optional callback used to free stored items.
 */
void linked_list_dispose(LinkedList** list, void (*free_function)(void*)) {
    linked_list_clear(*list, free_function);
    free(*list);
    *list = NULL;
}
