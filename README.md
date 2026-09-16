# uv

[![CI](https://github.com/alya-lang/uv/actions/workflows/ci.yml/badge.svg)](https://github.com/alya-lang/uv/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/alya-lang/uv?color=blue&label=License)](LICENSE)
[![Alya](https://img.shields.io/badge/dynamic/toml?url=https%3A%2F%2Fraw.githubusercontent.com%2Falya-lang%2Fuv%2Fmain%2Falya.toml&query=%24.package.alya-version&label=Alya&color=orange&prefix=%3E%3D)](https://github.com/alya-lang/alya)
[![Package Version](https://img.shields.io/badge/dynamic/toml?url=https%3A%2F%2Fraw.githubusercontent.com%2Falya-lang%2Fuv%2Fmain%2Falya.toml&query=%24.package.version&label=Version&color=brightgreen)](alya.toml)

High-performance, zero-dependency OS kernel I/O multiplexer and asynchronous event engine for Alya.

---

## 🌟 Features

- ⚡ **Kernel-Level Demultiplexing**: Direct native integration with OS notification mechanisms:
  - **Linux**: `epoll` (`epoll_create1`, `epoll_ctl`, `epoll_wait`)
  - **Windows**: `WSAPoll` / Winsock2
  - **macOS / BSD**: `kqueue` / POSIX fallback
- 🚀 **Extreme Concurrency (C100K Ready)**: Handles tens of thousands of active non-blocking socket connections with sub-microsecond event latency.
- 📦 **Zero External Dependencies**: Bundled C driver compiled automatically by `alyac` via native C FFI engine.
- 🎯 **Idiomatic Alya API**: First-class `UvEvent` enum, fluent `UvPoller` struct methods, and zero-allocation metadata passing (`udata`).

---

## 📁 Architecture

```text
uv/
├── alya.toml                  # Package manifest and C amalgamation configuration
├── c/
│   ├── alya_uv.h              # C interface header for native poller
│   └── alya_uv.c              # OS-specific kernel multiplexer implementation
├── src/
│   ├── lib.alya               # Public API facade
│   ├── types.alya             # UvEvent enum, UvPoller and UvEventNotification structs
│   ├── ffi.alya               # extern "C" declarations
│   └── core/
│       └── poller.alya        # High-level UvPoller struct methods and lifecycle
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
alyac add uv --git https://github.com/alya-lang/uv --branch main
alyac install
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
| `uv::version()` | None | `string` | Returns native uv engine version. |

### UvPoller Methods

| Method | Arguments | Returns | Description |
|---|---|---|---|
| `p.add(fd, events, udata)` | `fd: int, events: int, udata: int = 0` | `int` | Registers socket `fd` with the poller. Returns `0` on success. |
| `p.modify(fd, events, udata)` | `fd: int, events: int, udata: int = 0` | `int` | Modifies monitored event mask or `udata` for socket `fd`. |
| `p.remove(fd)` | `fd: int` | `int` | Deregisters socket `fd` from the poller. |
| `p.wait(timeout_ms)` | `timeout_ms: int = 100` | `array` | Waits for kernel events up to `timeout_ms`. Returns `[UvEventNotification]`. |
| `p.count()` | None | `int` | Returns number of active monitored sockets. |
| `p.close()` | None | `int` | Closes the poller and frees native OS resources. |

### Event Bitmasks (`UvEvent` Enum)

| Variant | Value | Description |
|---|---|---|
| `UvEvent.Readable` | `1` | Socket is ready to be read from without blocking. |
| `UvEvent.Writable` | `2` | Socket is ready to write data without blocking. |
| `UvEvent.Error` | `4` | Socket error condition detected by the kernel. |
| `UvEvent.Hangup` | `8` | Remote peer closed connection / EOF. |

---

## 🧪 Running Tests & Benchmarks

Run the test suite:

```bash
alyac test .
```

Run feature demonstration:

```bash
alyac run examples/demo.alya
```

Run micro-benchmarks:

```bash
alyac run benches/bench_basic.alya
```

---

## 📄 License

MIT License. Copyright (c) 2026 Alya Language Contributors.
