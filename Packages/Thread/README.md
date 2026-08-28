# Rux/Thread

Native OS thread management, execution control, and thread-local utilities for Rux.

## Features

- **Execution Control**: `Sleep`, `Yield`, `CurrentId`.
- **Cross-Platform**: Direct platform bindings on Windows (`CreateThread`), Linux (`sys_clone`), macOS and FreeBSD.
