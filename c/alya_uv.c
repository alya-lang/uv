#include "alya_uv.h"
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

/* --- Windows WSAPoll Backend --- */

struct alya_uv_poller {
    WSAPOLLFD* fds;
    int64_t* udata;
    int32_t count;
    int32_t capacity;
};

const char* alya_uv_backend_name(void) {
    return "WSAPoll";
}

alya_uv_poller_t* alya_uv_poller_create(int32_t initial_capacity) {
    if (initial_capacity < 8) {
        initial_capacity = 8;
    }
    alya_uv_poller_t* p = (alya_uv_poller_t*)malloc(sizeof(alya_uv_poller_t));
    if (!p) return NULL;
    p->fds = (WSAPOLLFD*)malloc(sizeof(WSAPOLLFD) * initial_capacity);
    p->udata = (int64_t*)malloc(sizeof(int64_t) * initial_capacity);
    if (!p->fds || !p->udata) {
        if (p->fds) free(p->fds);
        if (p->udata) free(p->udata);
        free(p);
        return NULL;
    }
    p->count = 0;
    p->capacity = initial_capacity;
    return p;
}

static short alya_events_to_poll(int32_t events) {
    short flags = 0;
    if (events & ALYA_UV_READABLE) {
        flags |= (POLLRDNORM | POLLRDBAND | POLLIN);
    }
    if (events & ALYA_UV_WRITABLE) {
        flags |= (POLLWRNORM | POLLOUT);
    }
    return flags;
}

static int32_t alya_poll_to_events(short revents) {
    int32_t flags = 0;
    if (revents & (POLLRDNORM | POLLRDBAND | POLLIN)) {
        flags |= ALYA_UV_READABLE;
    }
    if (revents & (POLLWRNORM | POLLOUT)) {
        flags |= ALYA_UV_WRITABLE;
    }
    if (revents & POLLERR) {
        flags |= ALYA_UV_ERROR;
    }
    if (revents & (POLLHUP | POLLNVAL)) {
        flags |= ALYA_UV_HANGUP;
    }
    return flags;
}

int32_t alya_uv_poller_add(alya_uv_poller_t* p, int64_t fd, int32_t events, int64_t udata) {
    if (!p || fd < 0) return -1;
    /* Check if socket already present */
    for (int32_t i = 0; i < p->count; i++) {
        if ((int64_t)p->fds[i].fd == fd) {
            p->fds[i].events = alya_events_to_poll(events);
            p->fds[i].revents = 0;
            p->udata[i] = udata;
            return 0;
        }
    }
    /* Grow capacity if required */
    if (p->count >= p->capacity) {
        int32_t new_cap = p->capacity * 2;
        WSAPOLLFD* new_fds = (WSAPOLLFD*)realloc(p->fds, sizeof(WSAPOLLFD) * new_cap);
        int64_t* new_udata = (int64_t*)realloc(p->udata, sizeof(int64_t) * new_cap);
        if (!new_fds || !new_udata) return -1;
        p->fds = new_fds;
        p->udata = new_udata;
        p->capacity = new_cap;
    }
    int32_t idx = p->count;
    p->fds[idx].fd = (SOCKET)fd;
    p->fds[idx].events = alya_events_to_poll(events);
    p->fds[idx].revents = 0;
    p->udata[idx] = udata;
    p->count++;
    return 0;
}

int32_t alya_uv_poller_modify(alya_uv_poller_t* p, int64_t fd, int32_t events, int64_t udata) {
    if (!p || fd < 0) return -1;
    for (int32_t i = 0; i < p->count; i++) {
        if ((int64_t)p->fds[i].fd == fd) {
            p->fds[i].events = alya_events_to_poll(events);
            p->fds[i].revents = 0;
            p->udata[i] = udata;
            return 0;
        }
    }
    return -1;
}

int32_t alya_uv_poller_remove(alya_uv_poller_t* p, int64_t fd) {
    if (!p || fd < 0) return -1;
    for (int32_t i = 0; i < p->count; i++) {
        if ((int64_t)p->fds[i].fd == fd) {
            /* O(1) removal by swapping with the last element */
            int32_t last = p->count - 1;
            if (i != last) {
                p->fds[i] = p->fds[last];
                p->udata[i] = p->udata[last];
            }
            p->count--;
            return 0;
        }
    }
    return -1;
}

int32_t alya_uv_poller_wait(alya_uv_poller_t* p, alya_uv_event_t* out_events, int32_t max_events, int32_t timeout_ms) {
    if (!p || !out_events || max_events <= 0) return -1;
    if (p->count == 0) {
        if (timeout_ms > 0) {
            Sleep(timeout_ms);
        }
        return 0;
    }
    int res = WSAPoll(p->fds, (ULONG)p->count, timeout_ms);
    if (res <= 0) {
        return res; /* 0 on timeout, negative on error */
    }
    int32_t out_count = 0;
    for (int32_t i = 0; i < p->count && out_count < max_events; i++) {
        if (p->fds[i].revents != 0) {
            out_events[out_count].fd = (int64_t)p->fds[i].fd;
            out_events[out_count].events = alya_poll_to_events(p->fds[i].revents);
            out_events[out_count].udata = p->udata[i];
            p->fds[i].revents = 0;
            out_count++;
        }
    }
    return out_count;
}

int32_t alya_uv_poller_count(alya_uv_poller_t* p) {
    return p ? p->count : 0;
}

void alya_uv_poller_close(alya_uv_poller_t* p) {
    if (!p) return;
    if (p->fds) free(p->fds);
    if (p->udata) free(p->udata);
    free(p);
}

#elif defined(__linux__)

/* --- Linux epoll Backend --- */

#include <sys/epoll.h>
#include <unistd.h>

typedef struct alya_uv_epoll_slot {
    int64_t fd;
    int64_t udata;
} alya_uv_epoll_slot_t;

struct alya_uv_poller {
    int epfd;
    int32_t count;
    int32_t capacity;
    alya_uv_epoll_slot_t** slots;
};

const char* alya_uv_backend_name(void) {
    return "epoll";
}

alya_uv_poller_t* alya_uv_poller_create(int32_t initial_capacity) {
    if (initial_capacity < 8) initial_capacity = 8;
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) return NULL;
    alya_uv_poller_t* p = (alya_uv_poller_t*)malloc(sizeof(alya_uv_poller_t));
    if (!p) {
        close(epfd);
        return NULL;
    }
    p->epfd = epfd;
    p->count = 0;
    p->capacity = initial_capacity;
    p->slots = (alya_uv_epoll_slot_t**)malloc(sizeof(alya_uv_epoll_slot_t*) * (size_t)initial_capacity);
    if (!p->slots) {
        close(epfd);
        free(p);
        return NULL;
    }
    return p;
}

static uint32_t alya_events_to_epoll(int32_t events) {
    uint32_t flags = 0;
    if (events & ALYA_UV_READABLE) flags |= EPOLLIN;
    if (events & ALYA_UV_WRITABLE) flags |= EPOLLOUT;
    return flags;
}

static int32_t alya_epoll_to_events(uint32_t ep_events) {
    int32_t flags = 0;
    if (ep_events & EPOLLIN)  flags |= ALYA_UV_READABLE;
    if (ep_events & EPOLLOUT) flags |= ALYA_UV_WRITABLE;
    if (ep_events & EPOLLERR) flags |= ALYA_UV_ERROR;
    if (ep_events & (EPOLLHUP | EPOLLRDHUP)) flags |= ALYA_UV_HANGUP;
    return flags;
}

int32_t alya_uv_poller_add(alya_uv_poller_t* p, int64_t fd, int32_t events, int64_t udata) {
    if (!p || fd < 0) return -1;
    /* Check if socket is already present */
    for (int32_t i = 0; i < p->count; i++) {
        if (p->slots[i]->fd == fd) {
            p->slots[i]->udata = udata;
            struct epoll_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.events = alya_events_to_epoll(events);
            ev.data.ptr = p->slots[i];
            return epoll_ctl(p->epfd, EPOLL_CTL_MOD, (int)fd, &ev);
        }
    }
    /* Grow capacity if required */
    if (p->count >= p->capacity) {
        int32_t new_cap = p->capacity * 2;
        alya_uv_epoll_slot_t** new_slots = (alya_uv_epoll_slot_t**)realloc(p->slots, sizeof(alya_uv_epoll_slot_t*) * (size_t)new_cap);
        if (!new_slots) return -1;
        p->slots = new_slots;
        p->capacity = new_cap;
    }
    alya_uv_epoll_slot_t* slot = (alya_uv_epoll_slot_t*)malloc(sizeof(alya_uv_epoll_slot_t));
    if (!slot) return -1;
    slot->fd = fd;
    slot->udata = udata;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = alya_events_to_epoll(events);
    ev.data.ptr = slot;

    int res = epoll_ctl(p->epfd, EPOLL_CTL_ADD, (int)fd, &ev);
    if (res != 0) {
        free(slot);
        return res;
    }
    p->slots[p->count++] = slot;
    return 0;
}

int32_t alya_uv_poller_modify(alya_uv_poller_t* p, int64_t fd, int32_t events, int64_t udata) {
    if (!p || fd < 0) return -1;
    for (int32_t i = 0; i < p->count; i++) {
        if (p->slots[i]->fd == fd) {
            p->slots[i]->udata = udata;
            struct epoll_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.events = alya_events_to_epoll(events);
            ev.data.ptr = p->slots[i];
            return epoll_ctl(p->epfd, EPOLL_CTL_MOD, (int)fd, &ev);
        }
    }
    return -1;
}

int32_t alya_uv_poller_remove(alya_uv_poller_t* p, int64_t fd) {
    if (!p || fd < 0) return -1;
    for (int32_t i = 0; i < p->count; i++) {
        if (p->slots[i]->fd == fd) {
            epoll_ctl(p->epfd, EPOLL_CTL_DEL, (int)fd, NULL);
            free(p->slots[i]);
            int32_t last = p->count - 1;
            if (i != last) {
                p->slots[i] = p->slots[last];
            }
            p->count--;
            return 0;
        }
    }
    return -1;
}

int32_t alya_uv_poller_wait(alya_uv_poller_t* p, alya_uv_event_t* out_events, int32_t max_events, int32_t timeout_ms) {
    if (!p || !out_events || max_events <= 0) return -1;
    if (p->count == 0) {
        if (timeout_ms > 0) {
            usleep((useconds_t)timeout_ms * 1000);
        }
        return 0;
    }
    struct epoll_event ep_events[64];
    int batch = max_events < 64 ? max_events : 64;
    int nfds = epoll_wait(p->epfd, ep_events, batch, timeout_ms);
    if (nfds <= 0) return nfds;
    for (int i = 0; i < nfds; i++) {
        alya_uv_epoll_slot_t* slot = (alya_uv_epoll_slot_t*)ep_events[i].data.ptr;
        if (slot) {
            out_events[i].fd = slot->fd;
            out_events[i].udata = slot->udata;
        } else {
            out_events[i].fd = -1;
            out_events[i].udata = 0;
        }
        out_events[i].events = alya_epoll_to_events(ep_events[i].events);
    }
    return nfds;
}

int32_t alya_uv_poller_count(alya_uv_poller_t* p) {
    return p ? p->count : 0;
}

void alya_uv_poller_close(alya_uv_poller_t* p) {
    if (!p) return;
    if (p->slots) {
        for (int32_t i = 0; i < p->count; i++) {
            if (p->slots[i]) free(p->slots[i]);
        }
        free(p->slots);
    }
    if (p->epfd >= 0) close(p->epfd);
    free(p);
}

#else

/* --- POSIX / kqueue / Generic Fallback --- */

#include <poll.h>

struct alya_uv_poller {
    struct pollfd* fds;
    int64_t* udata;
    int32_t count;
    int32_t capacity;
};

const char* alya_uv_backend_name(void) {
    return "poll";
}

alya_uv_poller_t* alya_uv_poller_create(int32_t initial_capacity) {
    if (initial_capacity < 8) initial_capacity = 8;
    alya_uv_poller_t* p = (alya_uv_poller_t*)malloc(sizeof(alya_uv_poller_t));
    if (!p) return NULL;
    p->fds = (struct pollfd*)malloc(sizeof(struct pollfd) * initial_capacity);
    p->udata = (int64_t*)malloc(sizeof(int64_t) * initial_capacity);
    if (!p->fds || !p->udata) {
        if (p->fds) free(p->fds);
        if (p->udata) free(p->udata);
        free(p);
        return NULL;
    }
    p->count = 0;
    p->capacity = initial_capacity;
    return p;
}

static short alya_events_to_poll(int32_t events) {
    short flags = 0;
    if (events & ALYA_UV_READABLE) flags |= (POLLIN | POLLPRI);
    if (events & ALYA_UV_WRITABLE) flags |= POLLOUT;
    return flags;
}

static int32_t alya_poll_to_events(short revents) {
    int32_t flags = 0;
    if (revents & (POLLIN | POLLPRI)) flags |= ALYA_UV_READABLE;
    if (revents & POLLOUT) flags |= ALYA_UV_WRITABLE;
    if (revents & POLLERR) flags |= ALYA_UV_ERROR;
    if (revents & (POLLHUP | POLLNVAL)) flags |= ALYA_UV_HANGUP;
    return flags;
}

int32_t alya_uv_poller_add(alya_uv_poller_t* p, int64_t fd, int32_t events, int64_t udata) {
    if (!p || fd < 0) return -1;
    for (int32_t i = 0; i < p->count; i++) {
        if ((int64_t)p->fds[i].fd == fd) {
            p->fds[i].events = alya_events_to_poll(events);
            p->fds[i].revents = 0;
            p->udata[i] = udata;
            return 0;
        }
    }
    if (p->count >= p->capacity) {
        int32_t new_cap = p->capacity * 2;
        struct pollfd* new_fds = (struct pollfd*)realloc(p->fds, sizeof(struct pollfd) * new_cap);
        int64_t* new_udata = (int64_t*)realloc(p->udata, sizeof(int64_t) * new_cap);
        if (!new_fds || !new_udata) return -1;
        p->fds = new_fds;
        p->udata = new_udata;
        p->capacity = new_cap;
    }
    int32_t idx = p->count;
    p->fds[idx].fd = (int)fd;
    p->fds[idx].events = alya_events_to_poll(events);
    p->fds[idx].revents = 0;
    p->udata[idx] = udata;
    p->count++;
    return 0;
}

int32_t alya_uv_poller_modify(alya_uv_poller_t* p, int64_t fd, int32_t events, int64_t udata) {
    if (!p || fd < 0) return -1;
    for (int32_t i = 0; i < p->count; i++) {
        if ((int64_t)p->fds[i].fd == fd) {
            p->fds[i].events = alya_events_to_poll(events);
            p->fds[i].revents = 0;
            p->udata[i] = udata;
            return 0;
        }
    }
    return -1;
}

int32_t alya_uv_poller_remove(alya_uv_poller_t* p, int64_t fd) {
    if (!p || fd < 0) return -1;
    for (int32_t i = 0; i < p->count; i++) {
        if ((int64_t)p->fds[i].fd == fd) {
            int32_t last = p->count - 1;
            if (i != last) {
                p->fds[i] = p->fds[last];
                p->udata[i] = p->udata[last];
            }
            p->count--;
            return 0;
        }
    }
    return -1;
}

int32_t alya_uv_poller_wait(alya_uv_poller_t* p, alya_uv_event_t* out_events, int32_t max_events, int32_t timeout_ms) {
    if (!p || !out_events || max_events <= 0) return -1;
    if (p->count == 0) {
        if (timeout_ms > 0) {
            usleep((useconds_t)timeout_ms * 1000);
        }
        return 0;
    }
    int res = poll(p->fds, (nfds_t)p->count, timeout_ms);
    if (res <= 0) return res;
    int32_t out_count = 0;
    for (int32_t i = 0; i < p->count && out_count < max_events; i++) {
        if (p->fds[i].revents != 0) {
            out_events[out_count].fd = (int64_t)p->fds[i].fd;
            out_events[out_count].events = alya_poll_to_events(p->fds[i].revents);
            out_events[out_count].udata = p->udata[i];
            p->fds[i].revents = 0;
            out_count++;
        }
    }
    return out_count;
}

int32_t alya_uv_poller_count(alya_uv_poller_t* p) {
    return p ? p->count : 0;
}

void alya_uv_poller_close(alya_uv_poller_t* p) {
    if (!p) return;
    if (p->fds) free(p->fds);
    if (p->udata) free(p->udata);
    free(p);
}

#endif

/* Common Memory Accessors for Alya FFI */

alya_uv_event_t* alya_uv_events_create(int32_t max_events) {
    if (max_events <= 0) max_events = 16;
    alya_uv_event_t* evs = (alya_uv_event_t*)calloc((size_t)max_events, sizeof(alya_uv_event_t));
    return evs;
}

int64_t alya_uv_event_fd(const alya_uv_event_t* events, int32_t index) {
    if (!events || index < 0) return -1;
    return events[index].fd;
}

int32_t alya_uv_event_flags(const alya_uv_event_t* events, int32_t index) {
    if (!events || index < 0) return 0;
    return events[index].events;
}

int64_t alya_uv_event_udata(const alya_uv_event_t* events, int32_t index) {
    if (!events || index < 0) return 0;
    return events[index].udata;
}

void alya_uv_events_free(alya_uv_event_t* events) {
    if (events) free(events);
}
