#include <errno.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "tls-io.h"

#define FULL_BUFFER_SIZE 1024
#define READER_BUFFER_SIZE 160
#define SLEEP_TIME 10
#define MAX_RETRIES_TO_START_READING 10
#define MAX_RETRIES_TO_STOP_READING 3

/**
 * Sleep for X milliseconds.
 *
 * @param long milliseconds -> Quantity in milliseconds.
 */
static int milliseconds_sleep(long milliseconds) {
    struct timespec ts;
    int res;

    // Distribute milliseconds into seconds and nanoseconds.
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;

    do {
        res = nanosleep(&ts, &ts);
    } while (res && errno == EINTR);

    return res;
}

/**
 * Treat all relevant errors generated by the SSL_read function.
 *
 * @param err -> Error obtained by SSL_get_error(...).
 * @param attempts_after_end_message -> Current attempt to verify end of
 * message.
 * @param total_bytes -> Quantity of bytes read in total.
 * @return -> -1 if read must end, 0 otherwise.
 */
int treat_SSL_read_error(int err, int *attempts_after_end_message,
                         int total_bytes, int read_bytes,
                         bool *end_connection) {

    if (read_bytes == 0) {
        if (total_bytes == 0)
            *end_connection = true;

        return -1;
    }

    if (err == SSL_ERROR_WANT_READ) {
        // SSL data is received in the form of frames. After a frame is
        // completed, the socket requires a few seconds to become ready for the
        // next read. Eventually, all frames will be processed, and the reading
        // process should conclude after exhausting all attempts.
        *attempts_after_end_message += 1;
        milliseconds_sleep(SLEEP_TIME);

        if (*attempts_after_end_message == MAX_RETRIES_TO_STOP_READING)
            return -1;
    } else if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_SYSCALL ||
               err == SSL_ERROR_SSL) {
        // Others errors should not be treated, but only returned.
        printf("(error) Error in read function!\n");
        return -1;
    }

    return 0;
}

/**
 * Occasionally, the first message may take a little time to become available
 * for reading. This function waits for a few milliseconds before checking if
 * the socket is ready for reading. After MAX_RETRIES_TO_START_READING attempts,
 * the function will return an error code to indicate that it was not possible
 * to retrieve a message.
 *
 * @param attempts_to_get_first_message -> Current attempt to read.
 * @return -> -1 if MAX_RETRIES, 0 otherwise.
 */
int wait_for_first_message(int *attempts_to_get_first_message) {
    *attempts_to_get_first_message += 1;
    milliseconds_sleep(SLEEP_TIME);

    if (*attempts_to_get_first_message == MAX_RETRIES_TO_START_READING)
        return -1;

    return 0;
}

/**
 * After reading data from the SSL connection, the body should be updated with
 * the new bytes. If there is not enough space in the body, a reallocation is
 * performed to double the available space.
 *
 * @param body -> Partial body of the message.
 * @param read_buffer -> Message read.
 * @param allocated_space -> Current allocated space.
 * @param total_bytes -> Total bytes read.
 * @param read_bytes -> Bytes read in this message.
 */
void save_read_data(char **body, const char *read_buffer, int *allocated_space,
                    int total_bytes, int read_bytes) {
    char *reallocated_body;

    if (*allocated_space - READER_BUFFER_SIZE >= total_bytes)
        memcpy(*body + total_bytes, read_buffer, read_bytes);
    else {
        // Duplicate body memory.
        *allocated_space *= 2;
        reallocated_body = (char *)realloc(*body, *allocated_space + 1);
        if (reallocated_body != NULL)
            *body = reallocated_body;

        memcpy(*body + total_bytes, read_buffer, read_bytes);
    }
}

/**
 * Read all sent data in a SSL connection.
 *
 * @param ssl -> SSL connection to be read.
 * @param total_bytes -> Total of bytes read.
 * @return -> Full message.
 */
char *read_data_from_ssl(SSL *ssl, bool *end_connection, int *total_bytes) {
    *total_bytes = 0;
    bool first_reading_done = false;
    int attempts_after_end_message = 0;
    int attempts_to_get_first_message = 0;
    int allocated_space = FULL_BUFFER_SIZE;
    int read_bytes;

    char *reallocated_body;

    char read_buffer[READER_BUFFER_SIZE + 1];
    char *body = (char *)malloc(allocated_space);

    do {
        read_bytes = SSL_read(ssl, read_buffer, READER_BUFFER_SIZE);
        read_buffer[read_bytes] = '\0';

        // If the read operation was successful:
        if (read_bytes > 0) {
            first_reading_done = true;
            attempts_after_end_message = 0;

            save_read_data(&body, read_buffer, &allocated_space, *total_bytes,
                           read_bytes);

            *total_bytes += read_bytes;
        } else {
            int err = SSL_get_error(ssl, read_bytes);

            if (!first_reading_done &&
                wait_for_first_message(&attempts_to_get_first_message) != -1)
                break;

            if (treat_SSL_read_error(err, &attempts_after_end_message,
                                     *total_bytes, read_bytes,
                                     end_connection) == -1)
                break;
        }
    } while (1);

    // Reallocate the memory using the necessary amount of space.
    if ((reallocated_body = (char *)realloc(body, *total_bytes + 1)) != NULL) {
        body = reallocated_body;
    }
    body[*total_bytes] = '\0';

    return body;
}

/**
 * Send a message in a SSL connection.
 *
 * @param ssl -> Destination of the message.
 * @param message -> Message to be sent.
 * @param total_bytes -> Total bytes to be set.
 * @return -> >=0 quantity of written bytes, -1 if error.
 */
int write_data_in_ssl(SSL *ssl, const char *message, int total_bytes) {

    size_t written;
    int ret;

    if ((ret = SSL_write_ex(ssl, message, total_bytes, &written)) == 0) {
        printf("(error) Failed to write HTTP request.\n");

        int status = SSL_get_error(ssl, ret);
        switch (status) {
        case SSL_ERROR_WANT_WRITE:
        case SSL_ERROR_WANT_READ:
            printf("(error) Write/read error.\n");
            return -1;
        default:
            printf("(error) Error within the SSL connection.\n");
            return -1;
        }
    }

    return (int)written;
}
