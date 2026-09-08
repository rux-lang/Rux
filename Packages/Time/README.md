# Time

Nanosecond durations, monotonic and wall clocks, Gregorian calendar values, and RFC 3339 text.

## What it provides

- **`Duration`** — a span of time exact to the nanosecond, normalized so the fraction is always in [0, 1e9) with
  the sign carried by the seconds: equality is bytewise, ordering is two comparisons, and arithmetic is a carry.
  All of it is checked — an overflow at the edge of a 292-billion-year range is a bug in the caller's arithmetic,
  and gets a report rather than a wrap that turns a timeout into a deadline in the past.
- **`Instant` and `Timestamp`** — monotonic readings for measuring elapsed time and UTC wall-clock moments for
  interchange. `SleepFor` waits against the platform clock without exposing its raw handles.
- **Calendar values** — validated `Date` and `TimeOfDay` values, fixed `UtcOffset`s, `DateTime` conversion, and checked
  date arithmetic over the proleptic Gregorian calendar.
- **RFC 3339** — strict parsing with an explicit leap-second fold and canonical formatting with trimmed fractions.

All public value receivers borrow with `&T`. The remaining raw pointers are writable scalar/aggregate output slots and
platform FFI addresses, never ownership handles. The package's value types are structurally `Copy`; descriptive and
fallible factories retain names such as `Now`, `FromSeconds`, and `New`.

## Waiting, and what is not here

`SleepFor` is the supported way to wait. It is a clock operation — it blocks the calling thread against the platform
clock — and it is the only one v0.1.0 offers, because v0.1.0 has no concurrency support at all. There is no thread
creation, no thread identifier, no yield, no lock, no atomic and no channel in any first-party package; see
[First-Party Packages](../../Docs/Packages.md#not-in-v010) for why the `Sync` and `Thread` packages were withdrawn
rather than shipped. A caller that needs those must supply them, and `SleepFor` does not imply that a second thread
exists to be woken.

## Installation

```sh
rux add Rux/Time
```

## Documentation

<https://rux-lang.dev/docs/api/time>

## License

Licensed under the [MIT License](LICENSE.md).
