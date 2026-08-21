# Time

Durations, monotonic clocks, and wall clocks.

## What it provides

- **`Duration`** — a span of time exact to the nanosecond, normalized so the fraction is always in [0, 1e9) with
  the sign carried by the seconds: equality is bytewise, ordering is two comparisons, and arithmetic is a carry.
  All of it is checked — an overflow at the edge of a 292-billion-year range is a bug in the caller's arithmetic,
  and gets a report rather than a wrap that turns a timeout into a deadline in the past.

More arrives with the rest of the phase: monotonic instants, wall-clock timestamps, and the proleptic-Gregorian
calendar types.

## Installation

```sh
rux add Rux/Time
```

## Planned surface

A `Duration` type, a monotonic clock for measuring elapsed time, and a wall clock for calendar time — built over the platform primitives the binding packages already expose: `ClockGetTime` and `Timespec` on [FreeBSD](../FreeBSD) and the [Linux](../Linux), `GetTimeOfDay` and `Timeval` on [macOS](../macOS), and `SystemTime` / `FileTime` on [Windows](../Windows).

## License

Licensed under the [MIT License](LICENSE.md).
