#ifndef WEATHER_SERVER_INSTANCE_H
#define WEATHER_SERVER_INSTANCE_H

#include "http_server_connection.h"

typedef struct {
    HTTPServerConnection* connection;
} WeatherServerInstance;

/**
 * @brief Initialize a WeatherServerInstance.
 *
 * Associates the instance with a specific HTTPServerConnection and
 * registers the instance's on_request callback for handling HTTP requests.
 *
 * @param instance   Pointer to the WeatherServerInstance to initialize.
 * @param connection Pointer to the HTTPServerConnection.
 *
 * @return 0 on success, non-zero on failure.
 */
int weather_server_instance_initiate(WeatherServerInstance* instance,
                                     HTTPServerConnection*  connection);

/**
 * @brief Allocate and initialize a WeatherServerInstance.
 *
 * Dynamically allocates a WeatherServerInstance, initializes it
 * using weather_server_instance_initiate(), and sets the output pointer.
 *
 * @param connection    Pointer to the HTTPServerConnection.
 * @param instance_ptr  Output pointer receiving the allocated instance.
 *
 * @return
 *   - 0 on success
 *   - -1 if instance_ptr is NULL
 *   - -2 if memory allocation fails
 *   - Any error returned by weather_server_instance_initiate()
 */
int weather_server_instance_initiate_ptr(HTTPServerConnection*   connection,
                                         WeatherServerInstance** instance_ptr);

/**
 * @brief Periodic work function for the instance.
 *
 * This function is called by the WeatherServer scheduler task
 * for each active WeatherServerInstance. Currently empty,
 * but can be used for timeouts, cleanup, or scheduled tasks.
 *
 * @param instance Pointer to the WeatherServerInstance.
 * @param mon_time Scheduler time in ticks.
 */
void weather_server_instance_work(WeatherServerInstance* instance,
                                  uint64_t               mon_time);

/**
 * @brief Dispose of a WeatherServerInstance.
 *
 * Cleans up any resources associated with the instance, such as
 * buffers or timers. Currently a no-op but reserved for future cleanup logic.
 *
 * @param instance Pointer to the WeatherServerInstance.
 */
void weather_server_instance_dispose(WeatherServerInstance* instance);

/**
 * @brief Destroy and free a dynamically allocated WeatherServerInstance.
 *
 * Calls weather_server_instance_dispose() and frees the memory.
 * Sets the caller's pointer to NULL.
 *
 * @param instance_ptr Pointer to a WeatherServerInstance pointer.
 */
void weather_server_instance_dispose_ptr(WeatherServerInstance** instance_ptr);

#endif // WEATHER_SERVER_INSTANCE_H
