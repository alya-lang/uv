# uv

[![CI](https://github.com/alya-lang/uv/actions/workflows/ci.yml/badge.svg)](https://github.com/alya-lang/uv/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/alya-lang/uv?color=blue&label=License)](LICENSE)
[![Alya](https://img.shields.io/badge/dynamic/toml?url=https%3A%2F%2Fraw.githubusercontent.com%2Falya-lang%2Fuv%2Fmain%2Falya.toml&query=%24.package.alya-version&label=Alya&color=orange&prefix=%3E%3D)](https://github.com/alya-lang/alya)
[![Package Version](https://img.shields.io/badge/dynamic/toml?url=https%3A%2F%2Fraw.githubusercontent.com%2Falya-lang%2Fuv%2Fmain%2Falya.toml&query=%24.package.version&label=Version&color=brightgreen)](alya.toml)

High-performance, zero-dependency OS kernel I/O multiplexer and asynchronous event engine for Alya.

---

## 🌟 Features

- ⚡ **Kernel-Level Demultiplexing**: Direct native integration with OS notification mechanisms:
  - **Linux**: `epoll` (`epoll_create1`, `epoll_ctl`, `epoll_wait`) + `signalfd`
  - **Windows**: `WSAPoll` / Winsock2 + native `IOCP` (I/O Completion Ports)
  - **macOS / BSD**: Native `kqueue` (`kqueue`, `kevent`) with zero-polling
- 🚀 **Extreme Concurrency (C100K Ready)**: Handles tens of thousands of active non-blocking socket connections with sub-microsecond event latency.
- 📦 **Zero External Dependencies**: Bundled C driver compiled automatically by `alya` via native C FFI engine.
- 🎯 **Idiomatic Alya API**: First-class `UvEvent` enum, fluent `UvPoller` struct methods, `UvIocp` completion queue, and zero-allocation metadata passing (`udata`).

---

## 📁 Project Architecture

```text
uv/
├── alya.toml                  # Package manifest and C amalgamation configuration
├── c/
│   ├── alya_uv.h              # C interface header for native poller & IOCP
│   └── alya_uv.c              # OS-specific kernel multiplexer (epoll, WSAPoll, kqueue, IOCP)
├── src/
│   ├── lib.alya               # Public API facade (poller, iocp, backend_name)
│   ├── types.alya             # UvEvent enum, UvPoller, UvIocp, and UvEventNotification structs
│   ├── ffi.alya               # extern "C" declarations
│   └── core/
│       └── poller.alya        # High-level UvPoller & UvIocp struct methods and lifecycle
├── tests/
│   └── test_basic.alya        # Comprehensive unit and integration test suite
├── examples/
│   └── demo.alya              # Non-blocking TCP echo server showcase
└── benches/
    └── bench_basic.alya       # Performance micro-benchmarks
```

---

## 📦 Installation

Add `uv` to the `[dependencies]` section in your `alya.toml`:

```toml
[dependencies]
uv = { git = "https://github.com/alya-lang/uv", branch = "main" }
```

Or install it directly via the Alya package CLI:

```bash
alya add uv --git https://github.com/alya-lang/uv --branch main
alya install
```

---

## 🚀 Quick Start

```alya
import "std/net"
import "uv"

function main()
    let p = uv::poller()

    # 1. Start a non-blocking TCP server
    let srv = tcp_listen(8080, 128)
    tcp_set_nonblocking(srv, 1)

    # 2. Register socket with the kernel poller
    p.add(srv, uv::UvEvent.Readable, 1)

    # 3. Wait for I/O events
    let events = p.wait(100)
    for ev in events
        if (ev.events & uv::UvEvent.Readable) != 0
            say "Incoming connection ready on socket fd={ev.fd}"
        end
    end

    p.close()
end

main()
```

---

## 📖 API Reference

### UvPoller Functions

| Function | Arguments | Returns | Description |
|---|---|---|---|
| `uv::poller(initial_capacity, max_events)` | `cap = 64, max = 64` | `UvPoller` | Creates a new kernel multiplexer poller instance. |
| `uv::backend_name()` | None | `string` | Returns active backend name (`"WSAPoll"`, `"epoll"`, `"kqueue"`). |
| `uv::iocp(max_threads)` | `max_threads = 0` | `UvIocp` | Creates an asynchronous I/O completion queue. |

### UvPoller Methods

| Method | Arguments | Returns | Description |
|---|---|---|---|
| `p.add(fd, events, udata)` | `fd: int, events: int, udata: int = 0` | `int` | Registers socket `fd` with the poller. Returns `0` on success. |
| `p.modify(fd, events, udata)` | `fd: int, events: int, udata: int = 0` | `int` | Modifies monitored event mask or `udata` for socket `fd`. |
| `p.remove(fd)` | `fd: int` | `int` | Deregisters socket `fd` from the poller. |
| `p.wait(timeout_ms)` | `timeout_ms: int = 100` | `array` | Waits for kernel events up to `timeout_ms`. Returns `[UvEventNotification]`. |
| `p.watch_signal(signum, udata)` | `signum: int, udata: int = 0` | `int` | Registers interest in process signal (e.g. `SIGINT` = 2). |
| `p.unwatch_signal(signum)` | `signum: int` | `int` | Deregisters interest in process signal. |
| `p.count()` | None | `int` | Returns number of active monitored sockets. |
| `p.close()` | None | `int` | Closes the poller and frees native OS resources. |

### UvIocp Methods (Completion Queue)

| Method | Arguments | Returns | Description |
|---|---|---|---|
| `q.associate(handle, key)` | `handle: int, key: int = 0` | `int` | Associates socket/file handle with completion port. |
| `q.post(bytes, key, udata)` | `bytes: int, key: int = 0, udata: int = 0` | `int` | Posts a manual completion packet to the queue. |
| `q.wait(timeout_ms)` | `timeout_ms: int = 100` | `array` | Dequeues completed packet within `timeout_ms`. |
| `q.close()` | None | `int` | Closes completion port and frees resources. |

### Event Bitmasks (`UvEvent` Enum)

| Variant | Value | Description |
|---|---|---|
| `UvEvent.Readable` | `1` | Socket is ready to be read from without blocking. |
| `UvEvent.Writable` | `2` | Socket is ready to write data without blocking. |
| `UvEvent.Error` | `4` | Socket error condition detected by the kernel. |
| `UvEvent.Hangup` | `8` | Remote peer closed connection / EOF. |
| `UvEvent.Signal` | `16` | OS process signal was delivered to the poller. |
| `UvEvent.Completed` | `32` | Asynchronous completion packet dequeued from IOCP. |

---

## 🧪 Running Tests & Benchmarks

Run the test suite:

```bash
alya test .
```

Run feature demonstration:

```bash
alya run examples/demo.alya
```

Run micro-benchmarks:

```bash
alya run benches/bench_basic.alya
```

---

## 🤝 Contributing

Contributions are welcome! Please follow these steps:

1. Fork the repository and clone it locally
2. Install dependencies:
   ```bash
   alya install
   ```
3. Create your feature branch (`git checkout -b feature/my-feature`)
4. Verify tests and formatting before opening a PR:
   ```bash
   alya test
   alya fmt . --check
   ```
5. Commit your changes (`git commit -m "feat: add feature"`) and open a Pull Request

---

## 📄 License

MIT License. Copyright (c) 2026 Alya Language Contributors.
