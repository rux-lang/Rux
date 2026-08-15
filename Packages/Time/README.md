# Time

Durations, monotonic clocks, and wall clocks.

> **Not implemented yet.** The package is published so its name and identity are reserved and its place in the standard set is fixed. `Src/Time.rux` is a placeholder that declares nothing, so importing from this package is an error rather than a silent no-op.

## Installation

```sh
rux add Rux/Time
```

## Planned surface

A `Duration` type, a monotonic clock for measuring elapsed time, and a wall clock for calendar time — built over the platform primitives the binding packages already expose: `ClockGetTime` and `Timespec` on [FreeBSD](../FreeBSD) and the [Linux](../Linux), `GetTimeOfDay` and `Timeval` on [macOS](../macOS), and `SystemTime` / `FileTime` on [Windows](../Windows).

## License

Licensed under the [MIT License](LICENSE.md).
