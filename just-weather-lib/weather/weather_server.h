#ifndef WEATHER_SERVER_H
#define WEATHER_SERVER_H

#include "http_server.h"
#include "linked_list.h"
#include "smw.h"

typedef struct {
    HTTPServer httpServer;

    LinkedList* instances;

    SmwTask* task;

} WeatherServer;

/**
 * @brief Initialize a WeatherServer.
 *
 * This function performs the following:
 *   - Initializes the embedded HTTP server.
 *   - Registers the callback for HTTP connections.
 *   - Creates a linked list for managing WeatherServerInstance objects.
 *   - Registers a scheduler task for periodic work.
 *
 * @param server Pointer to the WeatherServer to initialize.
 *
 * @return 0 on success, non-zero on failure.
 */
int weather_server_initiate(WeatherServer* server);

/**
 * @brief Allocate and initialize a WeatherServer.
 *
 * Dynamically allocates a WeatherServer structure and initializes it
 * using weather_server_initiate(). On failure, allocated memory is freed.
 *
 * @param server_ptr Output pointer that will receive the allocated server.
 *
 * @return
 *   - 0 on success
 *   - -1 if server_ptr is NULL
 *   - -2 if memory allocation fails
 *   - Any error returned by weather_server_initiate()
 */
int weather_server_initiate_ptr(WeatherServer** server_ptr);

/**
 * @brief Shut down the WeatherServer.
 *
 * This function stops the embedded HTTP server and destroys
 * the scheduler task.
 *
 * @param server Pointer to the WeatherServer.
 */
void weather_server_dispose(WeatherServer* server);

/**
 * @brief Destroy and free a dynamically allocated WeatherServer.
 *
 * This function shuts down the server, frees its memory, and
 * sets the caller's pointer to NULL.
 *
 * @param server_ptr Pointer to a WeatherServer pointer.
 */
void weather_server_dispose_ptr(WeatherServer** server_ptr);

#endif // WEATHER_SERVER_H
