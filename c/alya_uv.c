#include "alya_uv.h"
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

/* ═══════════════════════════════════════════════════════════════
   Windows WSAPoll & Native IOCP Engine
   ═══════════════════════════════════════════════════════════════ */

static volatile int g_alya_uv_ctrl_c_flag = 0;

static BOOL WINAPI alya_uv_console_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
        g_alya_uv_ctrl_c_flag = 1;
        return TRUE;
    }
    return FALSE;
}

struct alya_uv_poller {
    WSAPOLLFD* fds;
    int64_t* udata;
    int32_t count;
    int32_t capacity;
    int signal_watched;
    int32_t signal_num;
    int64_t signal_udata;
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
    p->fds = (WSAPOLLFD*)malloc(sizeof(WSAPOLLFD) * (size_t)initial_capacity);
    p->udata = (int64_t*)malloc(sizeof(int64_t) * (size_t)initial_capacity);
    if (!p->fds || !p->udata) {
        if (p->fds) free(p->fds);
        if (p->udata) free(p->udata);
        free(p);
        return NULL;
    }
    p->count = 0;
    p->capacity = initial_capacity;
    p->signal_watched = 0;
    p->signal_num = 0;
    p->signal_udata = 0;
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
        WSAPOLLFD* new_fds = (WSAPOLLFD*)realloc(p->fds, sizeof(WSAPOLLFD) * (size_t)new_cap);
        int64_t* new_udata = (int64_t*)realloc(p->udata, sizeof(int64_t) * (size_t)new_cap);
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

int32_t alya_uv_poller_watch_signal(alya_uv_poller_t* p, int32_t signum, int64_t udata) {
    if (!p || signum <= 0) return -1;
    p->signal_watched = 1;
    p->signal_num = signum;
    p->signal_udata = udata;
    SetConsoleCtrlHandler(alya_uv_console_handler, TRUE);
    return 0;
}

int32_t alya_uv_poller_unwatch_signal(alya_uv_poller_t* p, int32_t signum) {
    if (!p || signum <= 0) return -1;
    if (p->signal_watched && p->signal_num == signum) {
        p->signal_watched = 0;
        SetConsoleCtrlHandler(alya_uv_console_handler, FALSE);
        return 0;
    }
    return -1;
}

int32_t alya_uv_poller_wait(alya_uv_poller_t* p, alya_uv_event_t* out_events, int32_t max_events, int32_t timeout_ms) {
    if (!p || !out_events || max_events <= 0) return -1;

    int32_t out_count = 0;

    /* Check console signal */
    if (p->signal_watched && g_alya_uv_ctrl_c_flag) {
        g_alya_uv_ctrl_c_flag = 0;
        out_events[out_count].fd = (int64_t)p->signal_num;
        out_events[out_count].events = ALYA_UV_SIGNAL;
        out_events[out_count].udata = p->signal_udata;
        out_count++;
        if (out_count >= max_events) return out_count;
    }

    if (p->count == 0) {
        if (timeout_ms > 0) {
            Sleep((DWORD)timeout_ms);
        }
        return out_count;
    }

    for (int32_t z = 0; z < p->count; z++) { p->fds[z].revents = 0; }
    int res = WSAPoll(p->fds, (ULONG)p->count, timeout_ms);
    if (res <= 0) {
        return out_count > 0 ? out_count : res;
    }

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
    if (p->signal_watched) {
        SetConsoleCtrlHandler(alya_uv_console_handler, FALSE);
    }
    if (p->fds) free(p->fds);
    if (p->udata) free(p->udata);
    free(p);
}

/* Native Win32 IOCP Implementation */

struct alya_uv_iocp {
    HANDLE port;
    int32_t count;
};

alya_uv_iocp_t* alya_uv_iocp_create(int32_t max_threads) {
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, (DWORD)max_threads);
    if (!port) return NULL;
    alya_uv_iocp_t* iocp = (alya_uv_iocp_t*)malloc(sizeof(alya_uv_iocp_t));
    if (!iocp) {
        CloseHandle(port);
        return NULL;
    }
    iocp->port = port;
    iocp->count = 0;
    return iocp;
}

int32_t alya_uv_iocp_associate(alya_uv_iocp_t* iocp, int64_t socket_handle, int64_t completion_key) {
    if (!iocp || !iocp->port || socket_handle <= 0) return -1;
    HANDLE h = CreateIoCompletionPort((HANDLE)socket_handle, iocp->port, (ULONG_PTR)completion_key, 0);
    if (!h) return -1;
    iocp->count++;
    return 0;
}

int32_t alya_uv_iocp_post(alya_uv_iocp_t* iocp, int64_t bytes_transferred, int64_t completion_key, int64_t udata) {
    if (!iocp || !iocp->port) return -1;
    BOOL ok = PostQueuedCompletionStatus(iocp->port, (DWORD)bytes_transferred, (ULONG_PTR)completion_key, (LPOVERLAPPED)(uintptr_t)udata);
    return ok ? 0 : -1;
}

int32_t alya_uv_iocp_wait(alya_uv_iocp_t* iocp, alya_uv_event_t* out_events, int32_t max_events, int32_t timeout_ms) {
    if (!iocp || !iocp->port || !out_events || max_events <= 0) return -1;
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED overlapped = NULL;
    DWORD wait_ms = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
    BOOL ok = GetQueuedCompletionStatus(iocp->port, &bytes, &key, &overlapped, wait_ms);
    if (!ok && !overlapped) {
        return 0; /* Timeout or failure */
    }
    out_events[0].fd = (int64_t)key;
    out_events[0].events = ALYA_UV_COMPLETED | (ok ? 0 : ALYA_UV_ERROR);
    out_events[0].udata = (int64_t)(uintptr_t)overlapped;
    return 1;
}

void alya_uv_iocp_close(alya_uv_iocp_t* iocp) {
    if (!iocp) return;
    if (iocp->port) CloseHandle(iocp->port);
    free(iocp);
}

#elif defined(__linux__)

/* ═══════════════════════════════════════════════════════════════
   Linux epoll, signalfd & Kernel Queue Engine
   ═══════════════════════════════════════════════════════════════ */

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

typedef struct alya_uv_epoll_slot {
    int64_t fd;
    int64_t udata;
    int is_signal;
    int signum;
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
    for (int32_t i = 0; i < p->count; i++) {
        if (!p->slots[i]->is_signal && p->slots[i]->fd == fd) {
            p->slots[i]->udata = udata;
            struct epoll_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.events = alya_events_to_epoll(events);
            ev.data.ptr = p->slots[i];
            return epoll_ctl(p->epfd, EPOLL_CTL_MOD, (int)fd, &ev);
        }
    }
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
    slot->is_signal = 0;
    slot->signum = 0;

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
        if (!p->slots[i]->is_signal && p->slots[i]->fd == fd) {
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
        if (!p->slots[i]->is_signal && p->slots[i]->fd == fd) {
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

int32_t alya_uv_poller_watch_signal(alya_uv_poller_t* p, int32_t signum, int64_t udata) {
    if (!p || signum <= 0) return -1;
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, signum);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd < 0) return -1;

    if (p->count >= p->capacity) {
        int32_t new_cap = p->capacity * 2;
        alya_uv_epoll_slot_t** new_slots = (alya_uv_epoll_slot_t**)realloc(p->slots, sizeof(alya_uv_epoll_slot_t*) * (size_t)new_cap);
        if (!new_slots) { close(sfd); return -1; }
        p->slots = new_slots;
        p->capacity = new_cap;
    }
    alya_uv_epoll_slot_t* slot = (alya_uv_epoll_slot_t*)malloc(sizeof(alya_uv_epoll_slot_t));
    if (!slot) { close(sfd); return -1; }
    slot->fd = (int64_t)sfd;
    slot->udata = udata;
    slot->is_signal = 1;
    slot->signum = signum;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = slot;
    int res = epoll_ctl(p->epfd, EPOLL_CTL_ADD, sfd, &ev);
    if (res != 0) {
        close(sfd);
        free(slot);
        return -1;
    }
    p->slots[p->count++] = slot;
    return 0;
}

int32_t alya_uv_poller_unwatch_signal(alya_uv_poller_t* p, int32_t signum) {
    if (!p || signum <= 0) return -1;
    for (int32_t i = 0; i < p->count; i++) {
        if (p->slots[i]->is_signal && p->slots[i]->signum == signum) {
            int sfd = (int)p->slots[i]->fd;
            epoll_ctl(p->epfd, EPOLL_CTL_DEL, sfd, NULL);
            close(sfd);
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
            if (slot->is_signal) {
                struct signalfd_siginfo fdsi;
                ssize_t s = read((int)slot->fd, &fdsi, sizeof(fdsi));
                (void)s;
                out_events[i].fd = (int64_t)slot->signum;
                out_events[i].events = ALYA_UV_SIGNAL;
                out_events[i].udata = slot->udata;
            } else {
                out_events[i].fd = slot->fd;
                out_events[i].udata = slot->udata;
                out_events[i].events = alya_epoll_to_events(ep_events[i].events);
            }
        } else {
            out_events[i].fd = -1;
            out_events[i].udata = 0;
            out_events[i].events = alya_epoll_to_events(ep_events[i].events);
        }
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
            if (p->slots[i]) {
                if (p->slots[i]->is_signal) {
                    close((int)p->slots[i]->fd);
                }
                free(p->slots[i]);
            }
        }
        free(p->slots);
    }
    if (p->epfd >= 0) close(p->epfd);
    free(p);
}

/* Linux Pipe-Backed Completion Queue */

struct alya_uv_iocp {
    int pipe_fds[2];
    int32_t count;
};

typedef struct alya_uv_iocp_msg {
    int64_t bytes;
    int64_t key;
    int64_t udata;
} alya_uv_iocp_msg_t;

alya_uv_iocp_t* alya_uv_iocp_create(int32_t max_threads) {
    (void)max_threads;
    alya_uv_iocp_t* iocp = (alya_uv_iocp_t*)malloc(sizeof(alya_uv_iocp_t));
    if (!iocp) return NULL;
    if (pipe(iocp->pipe_fds) != 0) {
        free(iocp);
        return NULL;
    }
    fcntl(iocp->pipe_fds[0], F_SETFL, O_NONBLOCK);
    iocp->count = 0;
    return iocp;
}

int32_t alya_uv_iocp_associate(alya_uv_iocp_t* iocp, int64_t socket_handle, int64_t completion_key) {
    (void)socket_handle;
    (void)completion_key;
    if (!iocp) return -1;
    iocp->count++;
    return 0;
}

int32_t alya_uv_iocp_post(alya_uv_iocp_t* iocp, int64_t bytes_transferred, int64_t completion_key, int64_t udata) {
    if (!iocp) return -1;
    alya_uv_iocp_msg_t msg;
    msg.bytes = bytes_transferred;
    msg.key = completion_key;
    msg.udata = udata;
    ssize_t written = write(iocp->pipe_fds[1], &msg, sizeof(msg));
    return written == sizeof(msg) ? 0 : -1;
}

int32_t alya_uv_iocp_wait(alya_uv_iocp_t* iocp, alya_uv_event_t* out_events, int32_t max_events, int32_t timeout_ms) {
    if (!iocp || !out_events || max_events <= 0) return -1;
    struct pollfd pfd;
    pfd.fd = iocp->pipe_fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) return 0;
    alya_uv_iocp_msg_t msg;
    ssize_t n = read(iocp->pipe_fds[0], &msg, sizeof(msg));
    if (n != sizeof(msg)) return 0;
    out_events[0].fd = msg.key;
    out_events[0].events = ALYA_UV_COMPLETED;
    out_events[0].udata = msg.udata;
    return 1;
}

void alya_uv_iocp_close(alya_uv_iocp_t* iocp) {
    if (!iocp) return;
    close(iocp->pipe_fds[0]);
    close(iocp->pipe_fds[1]);
    free(iocp);
}

#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

/* ═══════════════════════════════════════════════════════════════
   macOS & BSD Native kqueue Engine
   ═══════════════════════════════════════════════════════════════ */

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>

typedef struct alya_uv_kq_slot {
    int64_t fd;
    int32_t events;
    int64_t udata;
    int is_signal;
} alya_uv_kq_slot_t;

struct alya_uv_poller {
    int kq;
    int32_t count;
    int32_t capacity;
    alya_uv_kq_slot_t** slots;
};

const char* alya_uv_backend_name(void) {
    return "kqueue";
}

alya_uv_poller_t* alya_uv_poller_create(int32_t initial_capacity) {
    if (initial_capacity < 8) initial_capacity = 8;
    int kq = kqueue();
    if (kq < 0) return NULL;
    alya_uv_poller_t* p = (alya_uv_poller_t*)malloc(sizeof(alya_uv_poller_t));
    if (!p) {
        close(kq);
        return NULL;
    }
    p->kq = kq;
    p->count = 0;
    p->capacity = initial_capacity;
    p->slots = (alya_uv_kq_slot_t**)malloc(sizeof(alya_uv_kq_slot_t*) * (size_t)initial_capacity);
    if (!p->slots) {
        close(kq);
        free(p);
        return NULL;
    }
    return p;
}

int32_t alya_uv_poller_add(alya_uv_poller_t* p, int64_t fd, int32_t events, int64_t udata) {
    if (!p || fd < 0) return -1;
    for (int32_t i = 0; i < p->count; i++) {
        if (!p->slots[i]->is_signal && p->slots[i]->fd == fd) {
            return alya_uv_poller_modify(p, fd, events, udata);
        }
    }
    if (p->count >= p->capacity) {
        int32_t new_cap = p->capacity * 2;
        alya_uv_kq_slot_t** new_slots = (alya_uv_kq_slot_t**)realloc(p->slots, sizeof(alya_uv_kq_slot_t*) * (size_t)new_cap);
        if (!new_slots) return -1;
        p->slots = new_slots;
        p->capacity = new_cap;
    }
    alya_uv_kq_slot_t* slot = (alya_uv_kq_slot_t*)malloc(sizeof(alya_uv_kq_slot_t));
    if (!slot) return -1;
    slot->fd = fd;
    slot->events = events;
    slot->udata = udata;
    slot->is_signal = 0;

    struct kevent ch[2];
    int n = 0;
    if (events & ALYA_UV_READABLE) {
        EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, (void*)slot);
    }
    if (events & ALYA_UV_WRITABLE) {
        EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, (void*)slot);
    }
    if (n > 0) {
        int res = kevent(p->kq, ch, n, NULL, 0, NULL);
        if (res < 0) {
            free(slot);
            return -1;
        }
    }
    p->slots[p->count++] = slot;
    return 0;
}

int32_t alya_uv_poller_modify(alya_uv_poller_t* p, int64_t fd, int32_t events, int64_t udata) {
    if (!p || fd < 0) return -1;
    for (int32_t i = 0; i < p->count; i++) {
        if (!p->slots[i]->is_signal && p->slots[i]->fd == fd) {
            alya_uv_kq_slot_t* slot = p->slots[i];
            struct kevent ch[4];
            int n = 0;
            if ((events & ALYA_UV_READABLE) && !(slot->events & ALYA_UV_READABLE)) {
                EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, (void*)slot);
            } else if (!(events & ALYA_UV_READABLE) && (slot->events & ALYA_UV_READABLE)) {
                EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
            }
            if ((events & ALYA_UV_WRITABLE) && !(slot->events & ALYA_UV_WRITABLE)) {
                EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, (void*)slot);
            } else if (!(events & ALYA_UV_WRITABLE) && (slot->events & ALYA_UV_WRITABLE)) {
                EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
            }
            if (n > 0) {
                kevent(p->kq, ch, n, NULL, 0, NULL);
            }
            slot->events = events;
            slot->udata = udata;
            return 0;
        }
    }
    return -1;
}

int32_t alya_uv_poller_remove(alya_uv_poller_t* p, int64_t fd) {
    if (!p || fd < 0) return -1;
    for (int32_t i = 0; i < p->count; i++) {
        if (!p->slots[i]->is_signal && p->slots[i]->fd == fd) {
            alya_uv_kq_slot_t* slot = p->slots[i];
            struct kevent ch[2];
            int n = 0;
            if (slot->events & ALYA_UV_READABLE) {
                EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
            }
            if (slot->events & ALYA_UV_WRITABLE) {
                EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
            }
            if (n > 0) {
                kevent(p->kq, ch, n, NULL, 0, NULL);
            }
            free(slot);
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

int32_t alya_uv_poller_watch_signal(alya_uv_poller_t* p, int32_t signum, int64_t udata) {
    if (!p || signum <= 0) return -1;
    signal(signum, SIG_IGN);
    if (p->count >= p->capacity) {
        int32_t new_cap = p->capacity * 2;
        alya_uv_kq_slot_t** new_slots = (alya_uv_kq_slot_t**)realloc(p->slots, sizeof(alya_uv_kq_slot_t*) * (size_t)new_cap);
        if (!new_slots) return -1;
        p->slots = new_slots;
        p->capacity = new_cap;
    }
    alya_uv_kq_slot_t* slot = (alya_uv_kq_slot_t*)malloc(sizeof(alya_uv_kq_slot_t));
    if (!slot) return -1;
    slot->fd = (int64_t)signum;
    slot->events = ALYA_UV_SIGNAL;
    slot->udata = udata;
    slot->is_signal = 1;

    struct kevent ch;
    EV_SET(&ch, (uintptr_t)signum, EVFILT_SIGNAL, EV_ADD | EV_ENABLE, 0, 0, (void*)slot);
    int res = kevent(p->kq, &ch, 1, NULL, 0, NULL);
    if (res < 0) {
        free(slot);
        return -1;
    }
    p->slots[p->count++] = slot;
    return 0;
}

int32_t alya_uv_poller_unwatch_signal(alya_uv_poller_t* p, int32_t signum) {
    if (!p || signum <= 0) return -1;
    for (int32_t i = 0; i < p->count; i++) {
        if (p->slots[i]->is_signal && p->slots[i]->fd == signum) {
            struct kevent ch;
            EV_SET(&ch, (uintptr_t)signum, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
            kevent(p->kq, &ch, 1, NULL, 0, NULL);
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
    struct timespec ts;
    struct timespec* pts = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        pts = &ts;
    }
    struct kevent evs[64];
    int batch = max_events < 64 ? max_events : 64;
    int nfds = kevent(p->kq, NULL, 0, evs, batch, pts);
    if (nfds <= 0) return nfds;
    for (int i = 0; i < nfds; i++) {
        alya_uv_kq_slot_t* slot = (alya_uv_kq_slot_t*)evs[i].udata;
        if (slot) {
            out_events[i].fd = slot->fd;
            out_events[i].udata = slot->udata;
            if (slot->is_signal) {
                out_events[i].events = ALYA_UV_SIGNAL;
                continue;
            }
        } else {
            out_events[i].fd = (int64_t)evs[i].ident;
            out_events[i].udata = 0;
        }
        int32_t flags = 0;
        if (evs[i].filter == EVFILT_READ) flags |= ALYA_UV_READABLE;
        if (evs[i].filter == EVFILT_WRITE) flags |= ALYA_UV_WRITABLE;
        if (evs[i].flags & EV_EOF) flags |= ALYA_UV_HANGUP;
        if (evs[i].flags & EV_ERROR) flags |= ALYA_UV_ERROR;
        out_events[i].events = flags;
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
    if (p->kq >= 0) close(p->kq);
    free(p);
}

/* macOS / BSD Pipe-Backed Completion Queue */

struct alya_uv_iocp {
    int pipe_fds[2];
    int32_t count;
};

typedef struct alya_uv_iocp_msg {
    int64_t bytes;
    int64_t key;
    int64_t udata;
} alya_uv_iocp_msg_t;

alya_uv_iocp_t* alya_uv_iocp_create(int32_t max_threads) {
    (void)max_threads;
    alya_uv_iocp_t* iocp = (alya_uv_iocp_t*)malloc(sizeof(alya_uv_iocp_t));
    if (!iocp) return NULL;
    if (pipe(iocp->pipe_fds) != 0) {
        free(iocp);
        return NULL;
    }
    fcntl(iocp->pipe_fds[0], F_SETFL, O_NONBLOCK);
    iocp->count = 0;
    return iocp;
}

int32_t alya_uv_iocp_associate(alya_uv_iocp_t* iocp, int64_t socket_handle, int64_t completion_key) {
    (void)socket_handle;
    (void)completion_key;
    if (!iocp) return -1;
    iocp->count++;
    return 0;
}

int32_t alya_uv_iocp_post(alya_uv_iocp_t* iocp, int64_t bytes_transferred, int64_t completion_key, int64_t udata) {
    if (!iocp) return -1;
    alya_uv_iocp_msg_t msg;
    msg.bytes = bytes_transferred;
    msg.key = completion_key;
    msg.udata = udata;
    ssize_t written = write(iocp->pipe_fds[1], &msg, sizeof(msg));
    return written == sizeof(msg) ? 0 : -1;
}

int32_t alya_uv_iocp_wait(alya_uv_iocp_t* iocp, alya_uv_event_t* out_events, int32_t max_events, int32_t timeout_ms) {
    if (!iocp || !out_events || max_events <= 0) return -1;
    struct pollfd pfd;
    pfd.fd = iocp->pipe_fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) return 0;
    alya_uv_iocp_msg_t msg;
    ssize_t n = read(iocp->pipe_fds[0], &msg, sizeof(msg));
    if (n != sizeof(msg)) return 0;
    out_events[0].fd = msg.key;
    out_events[0].events = ALYA_UV_COMPLETED;
    out_events[0].udata = msg.udata;
    return 1;
}

void alya_uv_iocp_close(alya_uv_iocp_t* iocp) {
    if (!iocp) return;
    close(iocp->pipe_fds[0]);
    close(iocp->pipe_fds[1]);
    free(iocp);
}

#else

/* ═══════════════════════════════════════════════════════════════
   POSIX poll Fallback Engine
   ═══════════════════════════════════════════════════════════════ */

#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

struct alya_uv_poller {
    struct pollfd* fds;
    int64_t* udata;
    int32_t count;
    int32_t capacity;
    int signal_watched;
    int32_t signal_num;
    int64_t signal_udata;
};

const char* alya_uv_backend_name(void) {
    return "poll";
}

alya_uv_poller_t* alya_uv_poller_create(int32_t initial_capacity) {
    if (initial_capacity < 8) initial_capacity = 8;
    alya_uv_poller_t* p = (alya_uv_poller_t*)malloc(sizeof(alya_uv_poller_t));
    if (!p) return NULL;
    p->fds = (struct pollfd*)malloc(sizeof(struct pollfd) * (size_t)initial_capacity);
    p->udata = (int64_t*)malloc(sizeof(int64_t) * (size_t)initial_capacity);
    if (!p->fds || !p->udata) {
        if (p->fds) free(p->fds);
        if (p->udata) free(p->udata);
        free(p);
        return NULL;
    }
    p->count = 0;
    p->capacity = initial_capacity;
    p->signal_watched = 0;
    p->signal_num = 0;
    p->signal_udata = 0;
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
        struct pollfd* new_fds = (struct pollfd*)realloc(p->fds, sizeof(struct pollfd) * (size_t)new_cap);
        int64_t* new_udata = (int64_t*)realloc(p->udata, sizeof(int64_t) * (size_t)new_cap);
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

int32_t alya_uv_poller_watch_signal(alya_uv_poller_t* p, int32_t signum, int64_t udata) {
    if (!p || signum <= 0) return -1;
    p->signal_watched = 1;
    p->signal_num = signum;
    p->signal_udata = udata;
    return 0;
}

int32_t alya_uv_poller_unwatch_signal(alya_uv_poller_t* p, int32_t signum) {
    if (!p || signum <= 0) return -1;
    if (p->signal_watched && p->signal_num == signum) {
        p->signal_watched = 0;
        return 0;
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

/* POSIX Generic Pipe-Backed Completion Queue */

struct alya_uv_iocp {
    int pipe_fds[2];
    int32_t count;
};

typedef struct alya_uv_iocp_msg {
    int64_t bytes;
    int64_t key;
    int64_t udata;
} alya_uv_iocp_msg_t;

alya_uv_iocp_t* alya_uv_iocp_create(int32_t max_threads) {
    (void)max_threads;
    alya_uv_iocp_t* iocp = (alya_uv_iocp_t*)malloc(sizeof(alya_uv_iocp_t));
    if (!iocp) return NULL;
    if (pipe(iocp->pipe_fds) != 0) {
        free(iocp);
        return NULL;
    }
    fcntl(iocp->pipe_fds[0], F_SETFL, O_NONBLOCK);
    iocp->count = 0;
    return iocp;
}

int32_t alya_uv_iocp_associate(alya_uv_iocp_t* iocp, int64_t socket_handle, int64_t completion_key) {
    (void)socket_handle;
    (void)completion_key;
    if (!iocp) return -1;
    iocp->count++;
    return 0;
}

int32_t alya_uv_iocp_post(alya_uv_iocp_t* iocp, int64_t bytes_transferred, int64_t completion_key, int64_t udata) {
    if (!iocp) return -1;
    alya_uv_iocp_msg_t msg;
    msg.bytes = bytes_transferred;
    msg.key = completion_key;
    msg.udata = udata;
    ssize_t written = write(iocp->pipe_fds[1], &msg, sizeof(msg));
    return written == sizeof(msg) ? 0 : -1;
}

int32_t alya_uv_iocp_wait(alya_uv_iocp_t* iocp, alya_uv_event_t* out_events, int32_t max_events, int32_t timeout_ms) {
    if (!iocp || !out_events || max_events <= 0) return -1;
    struct pollfd pfd;
    pfd.fd = iocp->pipe_fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) return 0;
    alya_uv_iocp_msg_t msg;
    ssize_t n = read(iocp->pipe_fds[0], &msg, sizeof(msg));
    if (n != sizeof(msg)) return 0;
    out_events[0].fd = msg.key;
    out_events[0].events = ALYA_UV_COMPLETED;
    out_events[0].udata = msg.udata;
    return 1;
}

void alya_uv_iocp_close(alya_uv_iocp_t* iocp) {
    if (!iocp) return;
    close(iocp->pipe_fds[0]);
    close(iocp->pipe_fds[1]);
    free(iocp);
}

#endif

/* ═══════════════════════════════════════════════════════════════
   Common Memory Accessors for Alya FFI
   ═══════════════════════════════════════════════════════════════ */

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
