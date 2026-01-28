/**
 * @file tls_client.h
 * @brief Non-blocking TLS client using mbedtls for secure connections.
 *
 * This module provides a TLS/SSL client implementation built on top of mbedtls.
 * It supports non-blocking connection establishment, certificate verification,
 * and encrypted I/O operations.
 *
 * The client wraps standard TCP socket operations with TLS encryption,
 * providing a simplified interface suitable for event-driven or scheduler-based
 * secure network programming.
 *
 * Key features:
 * - Non-blocking TLS handshake
 * - Certificate verification with configurable CA certificates
 * - Hostname validation (SNI)
 * - Support for both IPv4 and IPv6
 * - Automatic handling of TLS read/write buffering
 * - Simple, minimal API with explicit resource management
 *
 * All I/O operations are designed to work with non-blocking sockets and
 * return appropriate values to support integration with event loops, poll(),
 * select(), or scheduler task systems.
 *
 * @note The TLS handshake may require multiple calls to complete when using
 *       non-blocking mode. Callers should be prepared to retry operations
 *       when MBEDTLS_ERR_SSL_WANT_READ or MBEDTLS_ERR_SSL_WANT_WRITE occur.
 */

#ifndef TLS_CLIENT_H
#define TLS_CLIENT_H

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int                      fd;       // Socket file descriptor
    mbedtls_ssl_context      ssl;      // SSL/TLS context
    mbedtls_ssl_config       conf;     // SSL/TLS configuration
    mbedtls_entropy_context  entropy;  // Entropy context for RNG
    mbedtls_ctr_drbg_context ctr_drbg; // Deterministic random bit generator
    mbedtls_x509_crt         cacert;   // CA certificate chain
    int handshake_complete;            // Flag indicating handshake status
} TLSClient;

/**
 * @brief Initialize a TLS client structure.
 *
 * This function initializes all mbedtls contexts and prepares the TLS client
 * for use. It must be called before any other TLS client operations.
 *
 * @param c Pointer to the TLSClient structure to initialize.
 *
 * @return 0 on success, -1 on failure.
 *
 * @note After initialization, the client is ready for certificate loading
 *       and connection establishment.
 */
int tls_client_init(TLSClient* c);

/**
 * @brief Load CA certificates from a PEM string for certificate verification.
 *
 * This function loads one or more CA certificates in PEM format to be used
 * for verifying the server's certificate chain during the TLS handshake.
 *
 * @param c       Pointer to the initialized TLSClient.
 * @param ca_pem  Null-terminated string containing PEM-encoded CA
 * certificate(s).
 *
 * @return 0 on success, -1 on failure.
 *
 * @note This function should be called after tls_client_init() and before
 *       tls_client_connect(). If not called, certificate verification will
 * fail.
 */
int tls_client_load_ca_cert(TLSClient* c, const char* ca_pem);

/**
 * @brief Load CA certificates from a PEM file for certificate verification.
 *
 * This function loads CA certificates from a file path for verifying the
 * server's certificate chain during the TLS handshake.
 *
 * @param c         Pointer to the initialized TLSClient.
 * @param ca_path   Path to PEM-encoded CA certificate file.
 *
 * @return 0 on success, -1 on failure.
 */
int tls_client_load_ca_cert_file(TLSClient* c, const char* ca_path);

/**
 * @brief Create and initiate a non-blocking TLS connection to a remote host.
 *
 * This function resolves the given host and port, creates a TCP socket,
 * configures it for non-blocking operation, establishes a TCP connection,
 * and initiates the TLS handshake.
 *
 * The TLS handshake may not complete immediately in non-blocking mode.
 * The caller should use tls_client_handshake() to continue the handshake
 * process until it completes.
 *
 * @param c     Pointer to the initialized TLSClient.
 * @param host  Hostname or IP address of the remote peer (used for SNI).
 * @param port  Service name or numeric port (e.g. "443", "8443").
 *
 * @return
 *   - 0 if handshake completed immediately
 *   - 1 if handshake is in progress (caller should call tls_client_handshake)
 *   - -1 on fatal error
 *
 * @note The hostname is used for both connection and SNI (Server Name
 * Indication).
 */
int tls_client_connect(TLSClient* c, const char* host, const char* port);

/**
 * @brief Continue the TLS handshake process.
 *
 * This function should be called repeatedly after tls_client_connect() until
 * the handshake completes. It handles the non-blocking nature of the TLS
 * handshake protocol.
 *
 * @param c Pointer to the TLSClient with an in-progress handshake.
 *
 * @return
 *   - 0 if handshake completed successfully
 *   - 1 if handshake is still in progress (call again when socket is ready)
 *   - -1 on handshake failure
 *
 * @note When this returns 1, the caller should wait for the socket to become
 *       readable or writable (depending on mbedtls internal state) before
 *       calling again.
 */
int tls_client_handshake(TLSClient* c);

/**
 * @brief Send encrypted data over a TLS connection.
 *
 * This function sends up to @p len bytes from the provided buffer to the
 * remote peer using the established TLS connection.
 *
 * @param c    Pointer to the connected TLSClient with completed handshake.
 * @param buf  Buffer containing data to send.
 * @param len  Number of bytes to send.
 *
 * @return
 *   - Positive value: number of bytes sent
 *   - 0: operation would block (try again later)
 *   - -1: fatal error
 *
 * @note In non-blocking mode, this function may send fewer bytes than
 *       requested. The caller should handle partial writes.
 */
int tls_client_write(TLSClient* c, const uint8_t* buf, size_t len);

/**
 * @brief Receive encrypted data from a TLS connection.
 *
 * This function attempts to read up to @p len bytes from the TLS connection
 * into the provided buffer.
 *
 * @param c    Pointer to the connected TLSClient with completed handshake.
 * @param buf  Destination buffer for received data.
 * @param len  Maximum number of bytes to read.
 *
 * @return
 *   - Positive value: number of bytes read
 *   - 0: no data available (EAGAIN / EWOULDBLOCK) or would block
 *   - -2: peer has closed the connection (EOF)
 *   - -1: fatal read error
 *
 * @note This function distinguishes between temporary lack of data and
 *       connection closure, which is important for event-driven I/O.
 */
int tls_client_read(TLSClient* c, uint8_t* buf, size_t len);

/**
 * @brief Close the TLS connection associated with the client.
 *
 * This function performs a clean TLS shutdown by sending a close_notify
 * alert to the peer, then closes the underlying TCP socket.
 *
 * @param c Pointer to the TLSClient to disconnect.
 *
 * @note This function is safe to call even if the connection is already closed.
 */
void tls_client_disconnect(TLSClient* c);

/**
 * @brief Release all resources associated with a TLSClient.
 *
 * This function closes any open connection and frees all mbedtls contexts
 * and resources associated with the client.
 *
 * @param c Pointer to the TLSClient to dispose.
 *
 * @note After calling this function, the TLSClient structure should not be
 *       used without calling tls_client_init() again.
 */
void tls_client_dispose(TLSClient* c);

#endif // TLS_CLIENT_H
