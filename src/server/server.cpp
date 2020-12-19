#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <assert.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#include <signal.h>
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#include <cinttypes>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/utsname.h>  // for uname

#ifndef PB_ENABLE_MALLOC
#define PB_ENABLE_MALLOC
#endif
#include <pb.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include "mangohud.pb.h"

// Note, the connection can be established for debugging using socat:
// socat -d -d -d unix-connect:/tmp/9Lq7BNBnBycd6nxy.socket,type=5 stdio

#define LISTEN_BACKLOG 10
#define MAX_EVENTS 20

#include "common.h"

//#define DEBUG(a) do { } while(0)
#define DEBUG(a) do { a; } while(0)


// Helper for setting values in protobuf by pointer.
// Crude, but works.
template<typename T>
inline T* a(const T&& value) {
    T* ptr = (T*)malloc(sizeof(T));
    *ptr = value;
    return ptr;
}


struct PerClientServerState {
    int global_server_states_index;  // Index in `GlobalServerState::per_client_server_states` vector.

    struct RpcClientState rpc_client_state;

    Message recent_state;

    Message *request_change;
};

struct GlobalServerState {
    // TODO(baryluk): Maybe make it own all the elements,
    //     std::vector<PerClientServerState> per_client_server_states{};
    std::vector<PerClientServerState*> per_client_server_states{};
    bool server_shutdown = false;
};

struct RequestContext {
    // Server state assosciated with particular client.
    struct PerClientServerState *per_client_server_state;

    // We want to be able to access all the other clients easily
    // to server GUI type client.
    //
    // This is used for two purposes:
    //    For forwarding state of all non-GUI client to GUI clients.
    //    For forwarding GUI client request back to original client.
    struct GlobalServerState *global_server_state;
};

// Returns hostname aka nodename from gethostname or uname.
//
// Returned string is allocated by 'malloc' or NULL on failure.
static char* get_myhostname() {
    char hostname[HOST_NAME_MAX + 1];
    hostname[0] = '\0';
    if (gethostname(hostname, sizeof(hostname)) < 0) {
        perror("gethostname");
        hostname[HOST_NAME_MAX] = '\0'; // Just for a good measure.
    } else {
        return strdup(hostname);
    }
    if (strlen(hostname) == 0) {
        struct utsname utsname_buf;
        if (uname(&utsname_buf) < 0) {
            perror("uname");
            // What next?
        } else {
            return strdup(hostname);
        }
    }
    return NULL;
}

static int prepare_gui_response(Message* response, struct RequestContext *const context) {
    assert(PB_IF(context->per_client_server_state->recent_state.client_type, Message_ClientType_GUI));

    response->nodename = get_myhostname();  // Not PB_MALLOC_SET_STR!

    std::vector<Message> sub_responses;

    for (const auto& other_per_client_server_state : context->global_server_state->per_client_server_states) {
        if (context->per_client_server_state->global_server_states_index == other_per_client_server_state->global_server_states_index) {
            continue;
        }
        if (PB_IF(other_per_client_server_state->recent_state.client_type, Message_ClientType_GUI)) {
            continue;
        }

        //Message& sub_response = sub_responses.emplace_back();  // C++17
        sub_responses.emplace_back();
        Message& sub_response = sub_responses.back();

        // Populate nodename from the server (all reporting clients are
        // connected to server on local machine only).
        PB_MALLOC_SET_STR(sub_response.nodename, response->nodename);

        const Message* const recent_state = &(other_per_client_server_state->recent_state);

        // This would be so easier if the https://github.com/nanopb/nanopb/issues/173
        // was addressed.

        // Populate most recent stats for each client.
        PB_MAYBE_UPDATE(sub_response.uid, recent_state->uid);
        PB_MAYBE_UPDATE(sub_response.pid, recent_state->pid);
        PB_MAYBE_UPDATE_STR(sub_response.program_name, recent_state->program_name);
        PB_MAYBE_UPDATE_STR(sub_response.wine_version, recent_state->wine_version);
        PB_MAYBE_UPDATE(sub_response.fps, recent_state->fps);
        PB_MAYBE_UPDATE(sub_response.show_hud, recent_state->show_hud);

        if (recent_state->render_info) {
            PB_MALLOC_SET(sub_response.render_info, RenderInfo_init_zero);
            const RenderInfo* const render_info = recent_state->render_info;
            PB_MAYBE_UPDATE(sub_response.render_info->vulkan, render_info->vulkan);
            PB_MAYBE_UPDATE(sub_response.render_info->opengl, render_info->opengl);

            PB_MAYBE_UPDATE_STR(sub_response.render_info->engine_name, render_info->engine_name);
            PB_MAYBE_UPDATE_STR(sub_response.render_info->driver_name, render_info->driver_name);
            PB_MAYBE_UPDATE_STR(sub_response.render_info->gpu_name, render_info->gpu_name);

            PB_MAYBE_UPDATE(sub_response.render_info->opengl_version_major, render_info->opengl_version_major);
            PB_MAYBE_UPDATE(sub_response.render_info->opengl_version_minor, render_info->opengl_version_minor);
            PB_MAYBE_UPDATE(sub_response.render_info->opengl_is_gles, render_info->opengl_is_gles);

            PB_MAYBE_UPDATE(sub_response.render_info->vulkan_version_major, render_info->vulkan_version_major);
            PB_MAYBE_UPDATE(sub_response.render_info->vulkan_version_minor, render_info->vulkan_version_minor);
            PB_MAYBE_UPDATE(sub_response.render_info->vulkan_version_patch, render_info->vulkan_version_patch);
        }

    }

    // Note: It is actually safe to pass 0 to 'calloc', it will return NULL.
    PB_MALLOC_ARRAY(response->clients, sub_responses.size());
    for (size_t i = 0; i < sub_responses.size(); i++) {
         response->clients[i] = std::move(sub_responses[i]);
    }

    return 0;
}

// This can be used in request handler, but the pointer is not guaranteed to be
// valid after existing request handler.
// Use it only in the request handler and copy / modify data immedietly.
//
// Can return NULL.
//
// Do not free returned pointer.
//
// Message MUST contain nodename, uid and pid at least. And it should contain unique_in_process_id.
//
// TODO(baryluk): Don't crash if it doesn't.
static struct PerClientServerState* find_server_state_for_client(struct GlobalServerState* global_server_state, const Message *const message) {
    assert(message);
    assert(message->pid != NULL);
    assert(message->uid != NULL);
    assert(message->nodename != NULL);

    for (auto& per_client_server_state : global_server_state->per_client_server_states) {
         // TODO(baryluk): Right now we don't actually set the recent_state.nodename at all.
         //if (strcmp(per_client_server_state->recent_state.nodename, message->nodename) != 0) {
         //    continue;
         //}

         // TODO(baryluk): Check uid?

         if (PB_IF(per_client_server_state->recent_state.pid, *(message->pid))) {
             // Note, that it is possible to have multiple matches for the same pid.
             if (message->unique_in_process_id != NULL) {
                 if (PB_IF(per_client_server_state->recent_state.unique_in_process_id, *(message->unique_in_process_id))) {
                     return per_client_server_state;
                 } else {
                     continue;
                 }
             }
             // If unique_in_process_id is not set, return the first match for pid.
             return per_client_server_state;
             // TODO(baryluk): Or maybe return all of them?
         }
    }

    return NULL;
}


static int server_request_handler(const Message* const request, void* my_state) {
    // This is a bit circular, and not nice design, but should work.
    struct RequestContext *const context = (struct RequestContext*)my_state;

    struct PerClientServerState *const per_client_server_state = context->per_client_server_state;
    assert(per_client_server_state != NULL);

    Message *const recent_state = &(per_client_server_state->recent_state);

    // Debugging / sanity checks.
    // assert(server_state->client_state.connected);

    PB_MAYBE_UPDATE(recent_state->protocol_version, request->protocol_version);
    PB_MAYBE_UPDATE(recent_state->client_type, request->client_type);
    if (recent_state->render_info == NULL) {
        PB_MALLOC_SET(recent_state->timestamp, Timestamp_init_zero);
    }
    if (request->timestamp) {
        PB_MAYBE_UPDATE(recent_state->timestamp->clock_source, request->timestamp->clock_source);
        PB_MAYBE_UPDATE(recent_state->timestamp->timestamp_usec, request->timestamp->timestamp_usec);
    }

    uint64_t pid = 0;
    if (request->pid) {
        PB_MAYBE_UPDATE(recent_state->pid, request->pid);
        pid = *request->pid;
    }
    PB_MAYBE_UPDATE(recent_state->uid, request->uid);
    PB_MAYBE_UPDATE(recent_state->unique_in_process_id, request->unique_in_process_id);
    {
        if (recent_state->render_info == NULL) {
            PB_MALLOC_SET(recent_state->render_info, RenderInfo_init_zero);
        }
        char* type = "unknown";
        const RenderInfo *const render_info = request->render_info;
        if (render_info && render_info->opengl && *render_info->opengl) {
            type = "OpenGL";
            PB_MAYBE_UPDATE(recent_state->render_info->opengl, render_info->opengl);
            PB_MAYBE_UPDATE(recent_state->render_info->opengl_version_major, render_info->opengl_version_major);
            PB_MAYBE_UPDATE(recent_state->render_info->opengl_version_minor, render_info->opengl_version_minor);
            PB_MAYBE_UPDATE(recent_state->render_info->opengl_is_gles, render_info->opengl_is_gles);
        }
        if (render_info && render_info->vulkan && *render_info->vulkan) {
            type = "Vulkan";
            PB_MAYBE_UPDATE(recent_state->render_info->vulkan, render_info->vulkan);
            PB_MAYBE_UPDATE(recent_state->render_info->vulkan_version_major, render_info->vulkan_version_major);
            PB_MAYBE_UPDATE(recent_state->render_info->vulkan_version_minor, render_info->vulkan_version_minor);
            PB_MAYBE_UPDATE(recent_state->render_info->vulkan_version_patch, render_info->vulkan_version_patch);
        }
        char* engine_name = "";
        if (render_info && render_info->engine_name) {
            engine_name = render_info->engine_name;
            PB_MAYBE_UPDATE_STR(recent_state->render_info->engine_name, render_info->engine_name);
        }
        char* driver_name = "";
        if (render_info && render_info->driver_name) {
            driver_name = render_info->driver_name;
            PB_MAYBE_UPDATE_STR(recent_state->render_info->driver_name, render_info->driver_name);
        }
        if (render_info) {
            PB_MAYBE_UPDATE_STR(recent_state->render_info->gpu_name, render_info->gpu_name);
        }

        if (request->fps) {
            PB_MAYBE_UPDATE(recent_state->fps, request->fps);
            // fprintf(stderr, "pid %9lu   fps: %.3f  name=%s  type=%s engine=%s driver=%s\n", pid, *request->fps, request->program_name, type, engine_name, driver_name);
        }

        PB_MAYBE_UPDATE_STR(recent_state->program_name, request->program_name);
        PB_MAYBE_UPDATE_STR(recent_state->wine_version, request->wine_version);
        PB_MAYBE_UPDATE(recent_state->show_hud, request->show_hud);
    }


    //std::vector<uint32_t> frametimes;
    if (request->frametimes) {
        for (int i = 0; i < request->frametimes_count; i++) {
            //if (request->frametimes[i].time_usec) {
            //   //frametimes.push_back(*(request->frametimes[i].time_usec));
            //}
            //if (request->frametimes[i].index) {
            //   //frametimes_index.push_back(*(request->frametimes[i].index));
            //}
            const uint64_t timestamp_usec = request->frametimes[i].timestamp_usec ? *(request->frametimes[i].timestamp_usec) : 0;
            const uint32_t index = *(request->frametimes[i].index);
            const uint32_t time_usec = *(request->frametimes[i].time_usec);
            // fprintf(stderr, "   frame %9d: %7" PRIu32 " us @ %10" PRIu64 "\n", index, time_usec, timestamp_usec);
        }
    }
    //for (auto& frametime : frametimes) {
    //    fprintf(stderr, "   frame: %" PRIu32 "\n", frametime);
    //}

    // GUI can send back `client` with update config stuff (i.e. toggle HUD).
    if (request->clients && request->clients_count) {
        for (int i = 0; i < request->clients_count; i++) {
             struct PerClientServerState *per_client_server_state_for_client = find_server_state_for_client(context->global_server_state, &(request->clients[i]));
             if (per_client_server_state_for_client) {
                 if (per_client_server_state_for_client->request_change != NULL) {
                     // If other GUI client already requested change, or we didn't
                     // processed the previous one, override it.
                     pb_release(Message_fields, per_client_server_state_for_client->request_change);
                     free(per_client_server_state_for_client->request_change);
                     per_client_server_state_for_client->request_change = NULL;
                 }
                 PB_MALLOC_SET(per_client_server_state_for_client->request_change, Message_init_zero);
                 // Move ownership of pointer. LOL.
                 // This is really fishy. But lets try it.
                 // We need to do it by value, becasue
                 *(per_client_server_state_for_client->request_change) = std::move(request->clients[i]);
                 request->clients[i] = Message_init_zero;  // Zero out the struct, without freeing anything!
             }
        }
    }

    // Be very careful what you are doing here.
    // Don't mess with client_state, only set response if it is NULL.
    // Don't touch anything else (even reading).
    struct RpcClientState *rpc_client_state = &(per_client_server_state->rpc_client_state);
    if (rpc_client_state->response == NULL) {
        Message* response = (Message*)calloc(1, sizeof(Message));
        rpc_client_state->response = response;

        response->protocol_version = a<uint32_t>(1);
        response->client_type = a<Message_ClientType>(Message_ClientType_SERVER);

        if (PB_IF(request->client_type, Message_ClientType_APP)) {
            if (per_client_server_state->request_change) {
                Message* const request_change = per_client_server_state->request_change;

                if (request_change->show_hud != NULL) {
                    // DEBUG(fprintf(stderr, "change proxing back to client for show_hud: case 1\n"));
                    // Use swap, so we just steal the pointer with new malloc.
                    // Also swap old show_hud (if any), into request_change,
                    // so we free it if was set before.
                    std::swap(request_change->show_hud, response->show_hud);
                } else { // Proto3 Python workaround for lack of optional.
                    if (PB_IF(request_change->change_show_hud, true)) {
                        // DEBUG(fprintf(stderr, "change proxing back to client for show_hud: case 2\n"));
                        PB_MALLOC_SET(response->change_show_hud, true);
                        PB_MALLOC_SET(response->show_hud, false);
                    }
                }

                pb_release(Message_fields, per_client_server_state->request_change);
                free(per_client_server_state->request_change);
                per_client_server_state->request_change = NULL;
            }
        }

        if (PB_IF(request->client_type, Message_ClientType_GUI)) {
            if (prepare_gui_response(response, context) != 0) {
                return 1;  // Error.
            }
        }
    } else {
        fprintf(stderr, "Can not respond, some response is already set!\n");
    }

    return 0;
}

// There is no way around using some global variable with signal handlers.
static struct GlobalServerState *global_server_state_for_handler;

static void sigint_handler(int sig, siginfo_t *info, void *ucontext) {
    global_server_state_for_handler->server_shutdown = true;
}

static int loop(struct GlobalServerState* global_server_state) {
    // Create local socket.

retry_unix_socket:
    // int connection_socket = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    int connection_unix_socket = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (connection_unix_socket == -1) {
        if (errno == EINTR) {
            goto retry_unix_socket;
        }
        return errno;  // socket
    }

    // Construct dynamically the path to socket with user id.
    char socket_name[UNIX_PATH_MAX];  // 108 bytes on Linux. 92 on some weird systems.
    socket_name[0] = '\0';

    {
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


        {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));

        // Bind socket to socket name.

        addr.sun_family = AF_UNIX;
        assert(strlen(socket_name) >= 1);
        assert(strlen(socket_name) < sizeof(addr.sun_path) - 1);
        strncpy(addr.sun_path, socket_name, sizeof(addr.sun_path) - 1);
        // Force terminate, just in case there was truncation.
        addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
        assert(strlen(addr.sun_path) == strlen(socket_name));

        int retry = 1;
retry_unix_bind:
        if (bind(connection_unix_socket, (const struct sockaddr *)&addr,
                 sizeof(addr)) != 0) {
            if (errno == EADDRINUSE && retry > 0) {
                retry = 0;
                //if (connect(data_socket, (const struct sockaddr *) &addr,
                //            sizeof(addr));

                unlink(socket_name);
                goto retry_unix_bind;
            }
            return errno;  // bind
        }
        }
    }

retry_tcp_socket:
    // Should this be AF_INET6 or PF_INET6?
    int connection_tcp_socket = socket(AF_INET6, SOCK_STREAM, 0);
    if (connection_tcp_socket == -1) {
        if (errno == EINTR) {
            goto retry_tcp_socket;
        }
        return errno;  // socket
    }

    {
    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(sockaddr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(9869);
    //addr.sin6_addr = IN6ADDR_ANY_INIT;
    addr.sin6_addr = in6addr_any;

    int retry = 1;
retry_tcp_bind:
    if (bind(connection_tcp_socket, (const struct sockaddr *)&addr,
             sizeof(addr)) != 0) {
        if (errno == EADDRINUSE && retry > 0) {
            retry = 0;
            goto retry_tcp_bind;
        }
        return errno;  // bind
    }
    }

    {
    int reuse = 1;
    setsockopt(connection_tcp_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    }

    //{
    //int timestamp = 1;
    //setsockopt(connection_tcp_socket, SOL_SOCKET, SO_TIMESTAMP, &timestampe, sizeof(timestamp));
    //}


    if (listen(connection_unix_socket, LISTEN_BACKLOG) != 0) {
        return errno;  // listen
    }

    fprintf(stderr, "Listening on UNIX socket %s\n", socket_name);


    if (listen(connection_tcp_socket, LISTEN_BACKLOG) != 0) {
        return errno;  // listen
    }

    {
        struct sockaddr_in6 addr;
        memset(&addr, 0, sizeof(sockaddr));
        socklen_t addr_len = sizeof(addr);
        if (getsockname(connection_tcp_socket, (sockaddr*)&addr, &addr_len) == -1) {
            perror("getsockname");
        }
        assert(addr_len <= sizeof(addr));

        char addr_str[INET6_ADDRSTRLEN];
        addr_str[0] = '\0';
        if (inet_ntop(AF_INET6, &addr.sin6_addr, addr_str, sizeof(addr_str))) {
            fprintf(stderr, "Listening on TCP socket %s port %d\n", addr_str, ntohs(addr.sin6_port));
        } else {
            perror("inet_ntop");
        }
    }

    const int epollfd = epoll_create1(EPOLL_CLOEXEC);
    if (epollfd < 0) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    const int unix_socket_dummy_ptr = 0;
    const int tcp_socket_dummy_ptr = 0;

    {
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = (void*)&unix_socket_dummy_ptr;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, connection_unix_socket, &ev) == -1) {
        perror("epoll_ctl: connection_socket");
        exit(EXIT_FAILURE);
    }
    }

    {
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = (void*)&tcp_socket_dummy_ptr;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, connection_tcp_socket, &ev) == -1) {
        perror("epoll_ctl: connection_socket");
        exit(EXIT_FAILURE);
    }
    }

    while (!global_server_state->server_shutdown) {
        struct epoll_event events[MAX_EVENTS];

        const int nfds = epoll_pwait(epollfd, events, MAX_EVENTS, /*(int)timeout_ms=*/-1, /*sigmask*/NULL);
        if (nfds < 0 && errno == EINTR) {
            // Retry or exit if in shutdown.
            continue;
        }
        if (nfds < 0) {
            perror("epoll_pwait");
            exit(EXIT_FAILURE);
        }

        for (int n = 0; n < nfds; n++) {
            if (events[n].data.ptr == &unix_socket_dummy_ptr ||
                events[n].data.ptr == &tcp_socket_dummy_ptr) {

                struct sockaddr_in6 addr;
                memset(&addr, 0, sizeof(sockaddr));
                socklen_t addr_size = sizeof(addr);

                int data_socket =
                     events[n].data.ptr == &unix_socket_dummy_ptr
                     ? accept(connection_unix_socket, NULL, NULL)
                     : accept4(connection_tcp_socket, (sockaddr*)&addr, &addr_size, SOCK_CLOEXEC);

// accept4(connection_tcp_socket, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

                if (data_socket == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        continue;
                    }
                    if (events[n].data.ptr == &tcp_socket_dummy_ptr) {
                        if (errno == ENETDOWN || errno == EPROTO ||
                            errno == ENOPROTOOPT || errno == EHOSTDOWN ||
                            errno == ENONET || errno == EHOSTUNREACH ||
                            errno == EOPNOTSUPP || errno == ENETUNREACH) {
                            continue;
                        }
                    }
// EPERM Firewall rules forbid connection. (Linux)

                    perror("accept");
                    return errno;  // accept
                }

                fprintf(stderr, "Client connect started\n");

/*
       accept4() is a nonstandard Linux extension.

       On Linux, the new socket returned by accept() does not inherit file
       status flags such as O_NONBLOCK and O_ASYNC from the listening
       socket.  This behavior differs from the canonical BSD sockets
       implementation.  Portable programs should not rely on inheritance or
       noninheritance of file status flags and always explicitly set all
       required flags on the socket returned from accept().

*/
                if (events[n].data.ptr == &tcp_socket_dummy_ptr) {
                    char addr_str[INET6_ADDRSTRLEN];
                    addr_str[0] = '\0';
                    if (inet_ntop(AF_INET6, &addr.sin6_addr, addr_str, sizeof(addr_str))) {
                        fprintf(stderr, "TCP connection from %s port %d\n", addr_str, ntohs(addr.sin6_port));
                    } else {
                        perror("inet_ntop");
                    }
                }

                if (events[n].data.ptr == &tcp_socket_dummy_ptr) {
                    {
                    int keepalive = 1;
                    setsockopt(data_socket, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
                    }

                    {
                    struct linger no_linger;
                    no_linger.l_onoff = 0;
                    no_linger.l_linger = 0;  // seconds
                    setsockopt(data_socket, SOL_SOCKET, SO_LINGER, &no_linger, sizeof(no_linger));
                    }

                    {
                    struct sockaddr_in6 addr;
                    memset(&addr, 0, sizeof(addr));
                    socklen_t addr_len = sizeof(addr);
                    if (getpeername(data_socket, (sockaddr*)&addr, &addr_len) == -1) {
                        perror("getpeername");
                        exit(EXIT_FAILURE);
                    }
                    assert(addr_len == sizeof(struct sockaddr_in6));

                    char addr_str[INET6_ADDRSTRLEN];
                    addr_str[0] = '\0';
                    if (inet_ntop(AF_INET6, &addr.sin6_addr, addr_str, sizeof(addr_str))) {
                        fprintf(stderr, "TCP connection from %s port %d\n", addr_str, ntohs(addr.sin6_port));
                    } else {
                        perror("inet_ntop");
                    }
                    }
                }

// IP_TOS   , IPTOS_LOWDELAY



                if (set_nonblocking(data_socket)) {
                    goto error_1;
                }

                // Use extra scope to silence gcc's: jump to label ‘error_1’ crosses initialization of client_state / fsocket.
                {

                // We use libc FILE streams, for buffering.
                FILE* fsocket = fdopen(data_socket, "r");
                if (fsocket == NULL) {
                    perror("fdopen");
                    goto error_1;
                }

                struct PerClientServerState *const per_client_server_state = (struct PerClientServerState*)calloc(1, sizeof(struct PerClientServerState));
                // struct ServerState *const per_client_server_state = (struct PerClientServerState*)malloc(sizeof(struct PerClientServerState));
                // memset(per_client_server_state, 0, sizeof(struct PerClientServerState));

                struct RpcClientState *const rpc_client_state = &(per_client_server_state->rpc_client_state);

                rpc_client_state->client_type = 1;
                rpc_client_state->fd = data_socket;
                rpc_client_state->fsocket = fsocket;
                rpc_client_state->connected = 1;

                per_client_server_state->recent_state = Message_init_zero;

                per_client_server_state->global_server_states_index = global_server_state->per_client_server_states.size();

                global_server_state->per_client_server_states.push_back(per_client_server_state);

                struct epoll_event ev;
                // Note that is not needed to ever set EPOLLERR or EPOLLHUP,
                // they will always be reported.
                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = data_socket;
                ev.data.ptr = (void*)per_client_server_state;
                if (epoll_ctl(epollfd, EPOLL_CTL_ADD, data_socket, &ev) == -1) {
                    perror("epoll_ctl: data_socket");
                    exit(EXIT_FAILURE);
                }
                }

                fprintf(stderr, "Connected clients: %zu\n", global_server_state->per_client_server_states.size());

                continue;
error_1:
                fprintf(stderr, "Failure during creation of connection from client\n");
                if (data_socket < 0) {
                    continue;
                }
                if (close(data_socket)) {
                    perror("close");
                }
                continue;

            } else {
                // Data from client.
                struct PerClientServerState *const per_client_server_state = (struct PerClientServerState*)events[n].data.ptr;
                assert(per_client_server_state != NULL);
                struct RpcClientState *const rpc_client_state = &(per_client_server_state->rpc_client_state);

                struct RequestContext request_context = {0};
                request_context.per_client_server_state = per_client_server_state;
                request_context.global_server_state = global_server_state;

                // use_fd is fine to be called even if we got EPOLLERR or EPOLLHUP,
                // as rpc_client_use_fd is smart to handle properly read and
                // write errors, including returning 0, or other errors.
                if ((events[n].events & EPOLLERR) || (events[n].events & EPOLLHUP) ||
                    rpc_client_use_fd(rpc_client_state, server_request_handler, (void*)(&request_context))) {
                    fprintf(stderr, "Fatal error during handling client. Disconnecting.\n");
                    struct epoll_event dummy_event = {0};
                    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, rpc_client_state->fd, &dummy_event) == -1) {
                        perror("epoll_ctl: removing data_socket");
                        exit(EXIT_FAILURE);
                    }

                    // fclose is now power of client_state_cleanup.
                    //if (fclose(rpc_client_state->fsocket)) {
                    //    perror("fclose");
                    //}

                    int i = per_client_server_state->global_server_states_index;
                    assert(global_server_state->per_client_server_states.size() > 0);
                    assert(global_server_state->per_client_server_states[i] == per_client_server_state);
                    struct PerClientServerState *other_per_client_server_state = global_server_state->per_client_server_states.back();
                    if (other_per_client_server_state != per_client_server_state) {
                        assert(global_server_state->per_client_server_states.size() >= 1);
                        other_per_client_server_state->global_server_states_index = i;
                        global_server_state->per_client_server_states[i] = other_per_client_server_state;
                    }
                    global_server_state->per_client_server_states.pop_back();
                    fprintf(stderr, "New server_states vector size: %zu\n", global_server_state->per_client_server_states.size());

                    rpc_client_state_cleanup(rpc_client_state);
                    rpc_client_state->connected = 0;

                    pb_release(Message_fields, &(per_client_server_state->recent_state));

                    // free(per_client_server_state->client_state);
                    free(per_client_server_state);
                }
            }
        }
    }

    if (global_server_state->server_shutdown) {
        fprintf(stderr, "Received SIGINT (^C), shuting down\n");
    }

    // No need to do EPOLL_CTL_DEL on all connections.
    // Just close epollfd.
    if (close(epollfd) != 0) {
        perror("close: epollfd");
    }

    for (size_t i = 0; i < global_server_state->per_client_server_states.size(); i++) {
        printf("Closing %zu\n", i);
        struct PerClientServerState *const per_client_server_state = global_server_state->per_client_server_states[i];
        rpc_client_state_cleanup(&(per_client_server_state->rpc_client_state));

        pb_release(Message_fields, &(per_client_server_state->recent_state));

        // free(per_client_server_state->client_state);
        free(per_client_server_state);
    }
    global_server_state->per_client_server_states.clear();

close_tcp_socket:
    if (close(connection_tcp_socket) != 0) {
        perror("close: connection_tcp_socket");
    }

close_unix_socket:
    if (close(connection_unix_socket) != 0) {
        perror("close: connection_unix_socket");
    }

    if (strlen(socket_name) > 0) {
        fprintf(stderr, "Removing socket file %s\n", socket_name);
        if (unlink(socket_name) != 0) {
            perror("unlink: unix_socket file");
        }
    }

    return 0;
}



int main(int argc, const char** argv) {
    struct GlobalServerState global_server_state;
    global_server_state.server_shutdown = false;

    global_server_state_for_handler = &global_server_state;

    struct sigaction act_sigint = {0};
    act_sigint.sa_sigaction = &sigint_handler;
    act_sigint.sa_flags = SA_SIGINFO;
    struct sigaction oldact_sigint;
    if (sigaction(SIGINT, &act_sigint, &oldact_sigint) < 0) {
        perror("sigaction: SIGINT");
        return 1;
    }


    struct sigaction act_sigpipe = {0};
    act_sigpipe.sa_handler = SIG_IGN;
    struct sigaction oldact_sigpipe;
    if (sigaction(SIGPIPE, &act_sigpipe, &oldact_sigpipe) < 0) {
        perror("sigaction: SIGPIPE");
        return 1;
    }

    int ret = loop(&global_server_state);

    if (ret != 0) {
        errno = ret;
        perror("something");
        return 1;
    }

    fprintf(stderr, "Bye\n");

    return 0;
}
