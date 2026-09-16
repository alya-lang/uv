#ifndef ALYA_UV_H
#define ALYA_UV_H

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <windows.h>
  typedef SOCKET alya_socket_t;
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <unistd.h>
  typedef int alya_socket_t;
#endif

/* Event bitmasks */
#define ALYA_UV_READABLE  (1 << 0)  /* 1 */
#define ALYA_UV_WRITABLE  (1 << 1)  /* 2 */
#define ALYA_UV_ERROR     (1 << 2)  /* 4 */
#define ALYA_UV_HANGUP    (1 << 3)  /* 8 */

typedef struct alya_uv_event {
    int64_t fd;
    int32_t events;
    int64_t udata;
} alya_uv_event_t;

/* Forward declaration of poller handle */
typedef struct alya_uv_poller alya_uv_poller_t;

/* Public Poller Engine C API */
alya_uv_poller_t* alya_uv_poller_create(int32_t initial_capacity);
int32_t alya_uv_poller_add(alya_uv_poller_t* p, int64_t fd, int32_t events, int64_t udata);
int32_t alya_uv_poller_modify(alya_uv_poller_t* p, int64_t fd, int32_t events, int64_t udata);
int32_t alya_uv_poller_remove(alya_uv_poller_t* p, int64_t fd);
int32_t alya_uv_poller_wait(alya_uv_poller_t* p, alya_uv_event_t* out_events, int32_t max_events, int32_t timeout_ms);
int32_t alya_uv_poller_count(alya_uv_poller_t* p);
void alya_uv_poller_close(alya_uv_poller_t* p);

/* Event buffer allocation & accessors for safe Alya FFI */
alya_uv_event_t* alya_uv_events_create(int32_t max_events);
int64_t alya_uv_event_fd(const alya_uv_event_t* events, int32_t index);
int32_t alya_uv_event_flags(const alya_uv_event_t* events, int32_t index);
int64_t alya_uv_event_udata(const alya_uv_event_t* events, int32_t index);
void alya_uv_events_free(alya_uv_event_t* events);

/* Backend metadata */
const char* alya_uv_backend_name(void);
const char* alya_uv_version(void);

#endif /* ALYA_UV_H */
