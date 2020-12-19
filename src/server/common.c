#include "common.h"
#include <stdio.h>  // fprintf, fread
#include <stdlib.h>  // malloc, realloc, 
#include <arpa/inet.h>  // htonl, ntohl
#include <errno.h>  // errno, perror
#include <assert.h>  // assert
#include <unistd.h>  // SSIZE_MAX?, uint8_t?
#include <fcntl.h>  // fcntl
#include <sys/types.h>  // send
#include <sys/socket.h>  // send


// socket, setsockopt, connect
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <sys/un.h>  // AF_UNIX , sockaddr_un

// getpid()
#include <unistd.h>
#include <sys/types.h>  // Not really needed according to modern POSIX, or UNIX.

#include <string.h>  // memset, strncpy

#include <time.h>  // clock_gettime

#define PB_ENABLE_MALLOC
#include <pb.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include "mangohud.pb.h"

#define DEBUG(a) do { } while(0)
//#define DEBUG(a) do { a; } while(0)

#define MUST_USE_RESULT __attribute__ ((warn_unused_result))
#define COLD __attribute__ ((cold))
#define NOTNULL __attribute__ ((nonnull))
#define NOTHROW __attribute__ ((nothrow))

// TODO(baryluk): Set __attribute((visibility("hidden")) on some?

// static
int set_nonblocking(int fd) {
    // Set non-blocking.
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl: F_GETFL");
        return 1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl: F_SETFL +O_NONBLOCK");
        return 1;
    }
    return 0;
}

int rpc_client_connect(struct RpcClientState* rpc_client_state) {
    assert(rpc_client_state != NULL);
    if (rpc_client_state->connected) {
        fprintf(stderr, "Already connected!\n");
        return 0;
    }

    int data_socket = -1;

    // If the socket is alread present, assume we are continuing
    // asynchronous connect.
    if (rpc_client_state->fd == 0) {
        // Construct dynamically the path to socket with user id.
        char socket_name[UNIX_PATH_MAX];  // 108 bytes on Linux. 92 on some weird systems.
        {
        // I don't know better way of setting the format specifier to be more
        // portable than this. Some old glibc / Linux combos, and other OSes,
        // might have getuid() return uid_t that is only 16-bit.
        // Event casting to intmax_t (and using PRIdMAX ("%jd")), would ignore
        // sign-ness.
        //
        // See https://pubs.opengroup.org/onlinepubs/009695399/basedefs/sys/types.h.html for details.
        //
        // It usually is unsigned so lets do that. And usually 32-bit.
        // But, on mingw64 is is signed 64-bit, and on Solaris it was signed
        // 32-bit in the past. Unless you use gnulib, then it is 32-bit even
        // on mingw64.
        int ret = snprintf(socket_name, sizeof(socket_name), "/tmp/mangohud_server-%lu.sock", (unsigned long int)(getuid()));
        // None, of these should EVER happen. Ever.
        // But I like paranoia.
        if (ret < 0) {
            return 1;
        }
        if (ret <= 0) {
            return 1;
        }
        if (ret > 0 && (size_t)(ret) < strlen("/tmp/mangohud_server-1.sock")) {
            return 1;
        }
        if (ret > 0 && (size_t)(ret) >= UNIX_PATH_MAX - 1) {
            return 1;
        }
        }

retry_socket:
        // Create local socket.
        data_socket = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); // | SOCK_NONBLOCK, 0);
        // Note: For TCP sockets (AF_INET, AF_INET6 + SOCK_STREAM) we might want SOCK_KEEPALIVE.
        if (data_socket == -1) {
            if (errno == EINTR) {
                goto retry_socket;
            }

            perror("socket");
            return errno;  // socket
        }

#if defined(SO_NOSIGPIPE)
        // Available in BSD.
        const int set = 1;
        if (setsockopt(data_socket, SOL_SOCKET, SO_NOSIGPIPE, (void*)&set, sizeof(set)) < 0) {
            perror("setsockopt SOL_SOCKET SO_NOSIGPIPE");
            goto error_2;
        }
#endif

        // Usually one sets socket to be non-blocking after 'connect'.
        // But by making it earlier, we make the `connect` itself be
        // non-blocking too!
        if (set_nonblocking(data_socket)) {
            goto error_2;
        }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));

        addr.sun_family = AF_UNIX;
        assert(strlen(socket_name) >= 1);
        assert(strlen(socket_name) < sizeof(addr.sun_path) - 1);
        strncpy(addr.sun_path, socket_name, sizeof(addr.sun_path) - 1);
        // Force terminate, just in case there was truncation.
        addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
        assert(strlen(addr.sun_path) == strlen(socket_name));

        fprintf(stderr, "Connecting to server %s\n", socket_name);

retry_connect:
        if (connect(data_socket, (const struct sockaddr *)&addr,
                    sizeof(addr)) != 0) {
            if (errno != EINPROGRESS) {
                if (errno == EINTR) {
                    goto retry_connect;
                }
                perror("connect");
                DEBUG(fprintf(stderr, "The server is down?\n"));
                goto error_2;
            }
        }

        if (set_nonblocking(data_socket)) {
            goto error_2;
        }

        rpc_client_state->fd = data_socket;
    }

    assert(rpc_client_state->fd > 0);

    {
    FILE* fsocket = fdopen(rpc_client_state->fd, "r");
    if (fsocket == NULL) {
        perror("fdopen");
        goto error_2;
    }

    rpc_client_state->fsocket = fsocket;
    rpc_client_state->connected = 1;

    return 0;
    }

error_2:
    if (data_socket >= 0) {
        if (close(data_socket) != 0) {
            perror("close");
        }
    }
    rpc_client_state->fd = 0;
    return 1;
}


static int decode_and_handle(struct RpcClientState *rpc_client_state __attribute__((unused)),
                             const uint8_t *const data,
                             int data_size,
                             int(*request_handler)(const Message*, void*) MUST_USE_RESULT,
                             void *request_handler_state) {
    Message request = {0};
    pb_istream_t stream_input = pb_istream_from_buffer(data, data_size);

    // We don't need to use PB_ENCODE_DELIMITED, because it causes
    // complications in frameing and preallocation of stuff. It can be solved,
    // but just use simple format for now.
    if (!pb_decode_ex(&stream_input, Message_fields, &request, /*flags=*/0)) {
        DEBUG(fprintf(stderr, "decode or read failed: %s\n", stream_input.errmsg));
        // No need to call pb_release on error. pb_decode_ex takes care of that.
        return 1;
    }

    int ret = request_handler(&request, request_handler_state);

    // Required because we use PB_ENABLE_MALLOC and FT_POINTER for many fields.
    pb_release(Message_fields, &request);

    return -ret;
}


// 0 means, we finished reciving (at least the first packet).
// 0 also means, we executed the handler and it returned no errors.
//
// Positive means, there is more data to be received.
//
// Negative means there was network error, or serialization/deserialization
// error, or handler error.
//
// Caller needs to handle these properly.

#define ERROR_DONE 0
#define ERROR_MORE_NEEDED 1
#define ERROR_SYSTEM -1
#define ERROR_NETWORK -2
#define ERROR_SERIALIZE -3
#define ERROR_DESERIALIZE -4
#define ERROR_HANDLER(r) (-5-abs(r))

static int protocol_receive(struct RpcClientState* rpc_client_state,
                            int(*request_handler)(const Message*, void*) MUST_USE_RESULT,
                            void* request_handler_state) {
    DEBUG(fprintf(stderr, "protocol_receive\n"));
    FILE* file = rpc_client_state->fsocket;
    const size_t header_size = sizeof(uint32_t);
    assert(header_size == 4);
    if (rpc_client_state->input_frame_buffer_length < header_size) {
       const size_t ret = fread_unlocked(rpc_client_state->input_frame_buffer + rpc_client_state->input_frame_buffer_length,
                                         1,
                                         header_size - rpc_client_state->input_frame_buffer_length,
                                         file);
       const int ret_errno = errno;
       // This is a mess: https://pubs.opengroup.org/onlinepubs/9699919799/functions/fread.html
       if (ret <= 0) {
          if (ferror_unlocked(file)) {
              if (ret_errno == EAGAIN) {
                  return ERROR_MORE_NEEDED;
              }
              DEBUG(perror("fread_unlocked"));
              return ERROR_NETWORK;
          }
          return ERROR_SYSTEM;
       }
       rpc_client_state->input_frame_buffer_length += ret;
       assert(rpc_client_state->input_frame_buffer_length <= header_size);
       assert(0 <= rpc_client_state->input_frame_buffer_length);
       if (rpc_client_state->input_frame_buffer_length < header_size) {
           if (ret_errno == EAGAIN) {
              return ERROR_MORE_NEEDED;
           }
           return ERROR_SYSTEM;
       }
       if (rpc_client_state->input_frame_buffer_length == header_size) {
           DEBUG({
               const uint32_t s = ntohl(*((uint32_t*)(rpc_client_state->input_frame_buffer)));
               fprintf(stderr, "Got full framing size from client: %d\n", s);
           });
       }
    }
    if (rpc_client_state->input_frame_buffer_length != header_size) {
        return ERROR_MORE_NEEDED;
    }
    // Dereference the size, and if needed swap bytes to ensure correct endiannes.
    // const uint32_t input_serialized_size = ntohl(*((uint32_t*)(rpc_client_state->input_frame_buffer)));
    const uint32_t input_serialized_size = ntohl(rpc_client_state->input_frame_buffer_uint32);
    rpc_client_state->input_serialized_size = input_serialized_size;
    if (rpc_client_state->input_data_buffer == NULL || rpc_client_state->input_data_buffer_size == 0) {
        const size_t input_data_buffer_new_capacity = input_serialized_size * sizeof(uint8_t);
        rpc_client_state->input_data_buffer = (uint8_t*)realloc(rpc_client_state->input_data_buffer, input_data_buffer_new_capacity);
        rpc_client_state->input_data_buffer_capacity = input_data_buffer_new_capacity;
        rpc_client_state->input_data_buffer_size = 0;
    }
    assert(rpc_client_state->input_data_buffer != NULL);
    assert(rpc_client_state->input_data_buffer_capacity >= input_serialized_size);
    // Technically we can do a bit of looping here, especially if ret_errno
    // is EAGAIN, but we leave it to the higher level functions (`use_fd`
    // and `client_maybe_communicate`).
    if (rpc_client_state->input_data_buffer_size < input_serialized_size) {
       DEBUG(fprintf(stderr, "Still to read from client: %zu\n",
                             input_serialized_size - rpc_client_state->input_data_buffer_size));
       const size_t ret = fread_unlocked(rpc_client_state->input_data_buffer + rpc_client_state->input_data_buffer_size,
                                         1,
                                         input_serialized_size - rpc_client_state->input_data_buffer_size,
                                         file);
       const int ret_errno = errno;
       if (ret == 0) {
          if (ferror_unlocked(file)) {
              if (ret_errno == EAGAIN) {
                  // fclearerr_unlocked(file);
                  return ERROR_MORE_NEEDED;
              }
              DEBUG(perror("fread_unlocked"));
              return ERROR_NETWORK;
          }
          if (feof_unlocked(file)) {
              return ERROR_NETWORK;
          }
          return ERROR_SYSTEM;
       }
       assert(ret <= input_serialized_size - rpc_client_state->input_data_buffer_size);
       rpc_client_state->input_data_buffer_size += ret;
       assert(rpc_client_state->input_data_buffer_size <= input_serialized_size);
       assert(0 <= rpc_client_state->input_data_buffer_size);
       if (rpc_client_state->input_data_buffer_size < input_serialized_size) {
           if (ret_errno == EAGAIN) {
              return ERROR_MORE_NEEDED;
           }
           return ERROR_SYSTEM;
       }
    }
    assert(rpc_client_state->input_data_buffer_size == input_serialized_size);
    if (rpc_client_state->input_data_buffer_size == input_serialized_size) {
       const int ret = decode_and_handle(rpc_client_state, rpc_client_state->input_data_buffer, input_serialized_size, request_handler, request_handler_state);
       rpc_client_state->input_data_buffer_size = 0;
       rpc_client_state->input_frame_buffer_length = 0;
       rpc_client_state->input_serialized_size = 0;
       // We don't free the buffer, so we can reuse it on next frame.
       if (ret) {
         return ERROR_HANDLER(ret);  // or ERROR_DESERIALIZE
       } else {
         return ERROR_DONE;
       }
    }
    return ERROR_DONE;
}

static int protocol_send(struct RpcClientState* rpc_client_state) {
    DEBUG(fprintf(stderr, "protocol_send\n"));
    // Serialize response.
    if (rpc_client_state->output_send_remaining == 0) {
        assert(rpc_client_state->response != NULL);
        const Message* const message = rpc_client_state->response;
        size_t size = -1;
        if (!pb_get_encoded_size(&size, Message_fields, message)) {
            goto error_0;
        }
        assert(size >= 0);
        DEBUG(fprintf(stderr, "To be encoded size: %zu\n", size));
        // TODO(baryluk): Add some extra bytes for uint32_t alignment,
        // which some CPU architectures (arm, alpha, etc) might require!
        const size_t output_data_buffer_new_capacity = sizeof(uint32_t) + size * sizeof(uint8_t);
        rpc_client_state->output_data_buffer = (uint8_t*)realloc(rpc_client_state->output_data_buffer, output_data_buffer_new_capacity);
        {
            const uint32_t size_network_order = htonl((uint32_t)size);  // Convert to network byte order.
            // TODO(baryluk): Improve this by using proper alignment of output buffer.
            *((uint32_t*)(rpc_client_state->output_data_buffer)) = size_network_order;  // Write framing info at the start.
            // Put the message after framing info.
            pb_ostream_t stream_output = pb_ostream_from_buffer(rpc_client_state->output_data_buffer + sizeof(uint32_t), size);
            if (!pb_encode_ex(&stream_output, Message_fields, message, /*flags=*/0)) {
                DEBUG(fprintf(stderr, "encode failed: %s\n", stream_output.errmsg));
                return ERROR_SERIALIZE;
            }
            assert(size <= SSIZE_MAX - sizeof(uint32_t));
            rpc_client_state->output_serialized_size = size;  // This is excluding the frame header.
            rpc_client_state->output_send_remaining = size + sizeof(uint32_t);
            rpc_client_state->output_sent_already = 0;
        }

        pb_release(Message_fields, rpc_client_state->response);
        free(rpc_client_state->response);
        rpc_client_state->response = NULL;
    }

    // Start sending serialized response.
    {
    int eagain_count = 0;
    while (rpc_client_state->output_send_remaining > 0) {
        DEBUG(fprintf(stderr, "sending %zu bytes\n", rpc_client_state->output_send_remaining));
// TODO(baryluk): MSG_NOSIGNAL is Linux-specific I think.
// For BSD, just don't set it? On BSD we already set similar thing via SOL_SOCKET, SO_NOSIGPIPE.
        ssize_t write_bytes = send(rpc_client_state->fd,
                                   rpc_client_state->output_data_buffer + rpc_client_state->output_sent_already,
                                   rpc_client_state->output_send_remaining,
                                   MSG_NOSIGNAL);
        if (write_bytes < 0) {
            if (errno == EAGAIN) {
                eagain_count++;
                if (eagain_count < 2) {
                   continue;
                } else {
                   return ERROR_MORE_NEEDED;
                }
            }
            perror("send");
            return ERROR_SYSTEM;
        }
        assert(write_bytes <= rpc_client_state->output_send_remaining);
        rpc_client_state->output_send_remaining -= write_bytes;
        rpc_client_state->output_sent_already += write_bytes;
        assert(rpc_client_state->output_sent_already <= rpc_client_state->output_serialized_size + sizeof(uint32_t));
        assert(rpc_client_state->output_sent_already + rpc_client_state->output_send_remaining == rpc_client_state->output_serialized_size + sizeof(uint32_t));
        if (write_bytes == 0) {
            DEBUG(fprintf(stderr, "wrote 0 bytes out of %zu - will retry later\n", rpc_client_state->output_send_remaining));
            break;
        }
    }
    }

    // Note, that in case of error of working more on the data,
    // we don't mess with rpc_client_state->response, even if it is not NULL.
    // It is going to wait, and we will only process it next time,
    // once we finish with the current sending.
    if (rpc_client_state->output_send_remaining == 0) {
        DEBUG(fprintf(stderr, "sending done\n"));
        return ERROR_DONE;
    } else {
        DEBUG(fprintf(stderr, "sending will be continued later, remaining %zu bytes\n", rpc_client_state->output_send_remaining));
        return ERROR_MORE_NEEDED;
    }

    // We don't deallocate output_data_buffer even if we finished sending
    // everything. This is for reuse later.


    return ERROR_DONE;

error_0:
    if (rpc_client_state->response) {
        pb_release(Message_fields, rpc_client_state->response);
        free(rpc_client_state->response);
        rpc_client_state->response = NULL;
    }

    return 1;
}

// static
int rpc_client_use_fd(struct RpcClientState *rpc_client_state,
                      int(*request_handler)(const Message*, void*),
                      void *request_handler_state) {
    // If we are already receiving, continue receiving.
    // If we are not sending, but got an event, probably we got new data,
    // so receiving.
    if (rpc_client_state->in_receiving == 1 || (rpc_client_state->in_sending == 0 && rpc_client_state->response == NULL)) {
        DEBUG(fprintf(stderr, "in receiving\n"));
        int ret = protocol_receive(rpc_client_state, request_handler, request_handler_state);
        if (ret == 0) {
            // DONE
            rpc_client_state->in_receiving = 0;
            //return 0;
            goto try_sending_previous;
        }
        if (ret > 0) {
            // MORE NEEDED
            rpc_client_state->in_receiving = 1;
            // Still reciving, continue next time.
            goto try_sending_previous;
        }
        // If result is negative, that means network error happened, or there
        // was deserialization error, or handle_request returned error.
        fprintf(stderr, "error during receiving: ret = %d\n", ret);
        return ret;
    }

try_sending_previous:
    if (rpc_client_state->in_sending == 1) {
        DEBUG(fprintf(stderr, "in sending previous\n"));
        int ret = protocol_send(rpc_client_state);
        if (ret == 0) {
            // DONE
            rpc_client_state->in_sending = 0;
            goto try_responding;
        }
        if (ret > 0) {
            // MORE NEEDED.
            rpc_client_state->in_sending = 1;
            // Still in sending, just filled buffers and would block.
            return 0;
        }
        // Error occured (i.e. broken connection or pipe, serialization error).
        fprintf(stderr, "error during sending: ret = %d\n", ret);
        return 1;
    }

try_responding:
    if (rpc_client_state->in_sending == 0 && rpc_client_state->response != NULL) {
        DEBUG(fprintf(stderr, "in starting response\n"));
        int ret = protocol_send(rpc_client_state);
        if (ret == 0) {
            // DONE
            rpc_client_state->in_sending = 0;
            return 0;
        }
        if (ret > 0) {
            // MORE NEEDED.
            rpc_client_state->in_sending = 1;
            // Still in sending, just filled buffers and would block.
            return 0;
        }
        // Error occured (i.e. broken connection or pipe, serialization error).
        fprintf(stderr, "error during sending\n");
        return 1;
    }

    DEBUG(fprintf(stderr, "Nothing to do?\n"));

    return 0;
}

// TODO(baryluk): Add Windows support.
static uint64_t get_time_usec() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
        perror("clock_gettime CLOCK_MONOTONIC");
        exit(1);
    }
    return ts.tv_nsec/(uint64_t)(1000uL) + ts.tv_sec*(uint64_t)(1000000uL);
}

void rpc_client_maybe_communicate(struct RpcClientState *rpc_client_state,
                                  int(*message_generator)(Message*, void*),
                                  void* generator_state,
                                  int(*message_handler)(const Message*, void*),
                                  void* handler_state) {
   // assert(rpc_client_state != NULL);
   // if (rpc_client_state == NULL) {
   //   DEBUG(fprintf(stderr, "No client state!\n"));
   //   return;
   //}
   if (rpc_client_state->connected) {
       const uint64_t now_usec = get_time_usec();
       // Either generate a new message, or try reading any messages from server (or both).
       if (rpc_client_state->in_sending == 0 && rpc_client_state->response == NULL && rpc_client_state->last_send_time_usec + rpc_client_state->send_period_usec <= now_usec && message_generator != NULL) {
           Message* message = (Message*)calloc(1, sizeof(Message));

           message_generator(message, generator_state);

           rpc_client_state->response = message;  // PREPARE NEW STUFF FOR SENDING.

           // Technically this is not the time of send, but preparation. But close enough.
           rpc_client_state->last_send_time_usec = now_usec;
           rpc_client_state->send_period_usec = 200000;  // 200_000usec, 200ms
       }
       int retries = 0;
       int ret = -1;
       while ((ret = rpc_client_use_fd(rpc_client_state, message_handler, handler_state)) == 0) {
          retries++;
          if (retries >= 3) {
             break;
          }
       }
       if (ret != 0) {
          DEBUG(fprintf(stderr, "use_fd failed. Disconnecting and cleaning up.\n"));
          rpc_client_state_cleanup(rpc_client_state);
          assert(rpc_client_state->connected == 0);
       }
   } else {
       const uint64_t now_usec = get_time_usec();
       const uint64_t connection_retry_delay = 5000000; // 5s
       if (rpc_client_state->prev_connect_attempt_usec + connection_retry_delay <= now_usec) {
           DEBUG(fprintf(stderr, "Not connected, and retry delay passed. Trying to connect\n"));
           if (rpc_client_connect(rpc_client_state) == 0) {
               DEBUG(fprintf(stderr, "Connected! Yay!\n"));
               assert(rpc_client_state->connected == 1);
           } else {
               assert(rpc_client_state->connected == 0);
               rpc_client_state->prev_connect_attempt_usec = now_usec;
               DEBUG(fprintf(stderr, "Failed to connect. Will retry later.\n"));
           }
       }
   }
}

void rpc_client_state_cleanup(struct RpcClientState* rpc_client_state) {
    if (rpc_client_state->response != NULL) {
        pb_release(Message_fields, rpc_client_state->response);
        free(rpc_client_state->response);
        rpc_client_state->response = NULL;
    }
    rpc_client_state->input_frame_buffer_length = 0;

    rpc_client_state->input_data_buffer_size = 0;
    free(rpc_client_state->input_data_buffer);
    rpc_client_state->input_data_buffer = NULL;
    rpc_client_state->input_data_buffer_capacity = 0;

    rpc_client_state->output_data_buffer_size = 0;
    free(rpc_client_state->output_data_buffer);
    rpc_client_state->output_data_buffer = NULL;
    rpc_client_state->output_data_buffer_capacity = 0;

    rpc_client_state->connected = 0;

    rpc_client_state->input_serialized_size = 0;

    rpc_client_state->output_serialized_size = 0;
    rpc_client_state->output_send_remaining = 0;
    rpc_client_state->output_sent_already = 0;

    rpc_client_state->in_sending = 0;
    rpc_client_state->in_receiving = 0;

    if (rpc_client_state->fsocket) {
        if (fclose(rpc_client_state->fsocket) != 0) {
            perror("fclose");
        }
        rpc_client_state->fsocket = NULL;
        rpc_client_state->fd = 0;
    } else if (rpc_client_state->fd) {
        if (close(rpc_client_state->fd) != 0) {
            perror("close");
        }
        rpc_client_state->fd = 0;
    }

    rpc_client_state->last_send_time_usec = 0;
    rpc_client_state->send_period_usec = 200000;

    rpc_client_state->prev_connect_attempt_usec = 0;
}

#undef DEBUG
#undef MUST_USE_RESULT
