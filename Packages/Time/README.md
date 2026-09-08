# Time

Nanosecond durations, monotonic and wall clocks, Gregorian calendar values, and RFC 3339 text.

## What it provides

- **`Duration`** — a span of time exact to the nanosecond, normalized so the fraction is always in [0, 1e9) with the sign carried by the seconds: equality is bytewise, ordering is two comparisons, and arithmetic is a carry. All of it is checked — an overflow at the edge of a 292-billion-year range is a bug in the caller's arithmetic, and gets a report rather than a wrap that turns a timeout into a deadline in the past.
- **`Instant` and `Timestamp`** — monotonic readings for measuring elapsed time and UTC wall-clock moments for interchange. `SleepFor` waits against the platform clock without exposing its raw handles.
- **Calendar values** — validated `Date` and `TimeOfDay` values, fixed `UtcOffset`s, `DateTime` conversion, and checked date arithmetic over the proleptic Gregorian calendar.
- **RFC 3339 and the calendar shapes** — `ParseDate`, `ParseTimeOfDay`, `ParseDateTime` and `ParseRfc3339` each answer a `Result`, and every failure carries a zero-based byte offset into the text. The categories are the ones a caller acts on differently: a malformed shape, a month that is not one, a day the month does not have, a time out of range, a fraction too long, and an offset that is not one. Second 60 folds to the last nanosecond of second 59, and formatting never produces it, so the fold is one-way. `ParseDuration` reads the span form back the same way.
- **Presentation** — `Date`, `TimeOfDay`, `UtcOffset`, `DateTime`, `OffsetDateTime`, `Duration` and `Timestamp` implement `Display` and `Debug`. A `DateTime` writes no offset, because it holds none: `Z` would claim UTC and the machine's offset would claim a place. `OffsetDateTime` is the pairing that carries one and therefore the type that produces RFC 3339. A precision names 0 to 9 fractional digits and truncates; without one the fraction is written to the digits it has, trailing zeros trimmed.
- **One default per value** — a `Timestamp` is a moment on the calendar and a count of seconds at once, and only the first is what a bare placeholder writes: `{}` is RFC 3339 in UTC, and `{:unix}` is the signed second count. A moment outside years 0000 through 9999 has no RFC 3339 text and is refused with `ValueOutOfRange` before anything is written, which the second count still spells. `Instant` implements `Debug` alone, showing `Instant(nanoseconds: N)`: a monotonic reading counts from an origin the system chose at boot and has no text for a reader, only a difference from another reading.

All public value receivers borrow with `&T`. The remaining raw pointers are writable scalar/aggregate output slots and platform FFI addresses, never ownership handles. The package's value types are structurally `Copy`; descriptive and fallible factories retain names such as `Now`, `FromSeconds`, and `New`.

## Waiting, and what is not here

`SleepFor` is the supported way to wait. It is a clock operation — it blocks the calling thread against the platform clock — and it is the only one v0.1.0 offers, because v0.1.0 has no concurrency support at all. There is no thread creation, no thread identifier, no yield, no lock, no atomic and no channel in any first-party package; see [First-Party Packages](../../Docs/Packages.md#not-in-v010) for why the `Sync` and `Thread` packages were withdrawn rather than shipped. A caller that needs those must supply them, and `SleepFor` does not imply that a second thread exists to be woken.

## Installation

```sh
rux add Rux/Time
```

## Documentation

<https://rux-lang.dev/docs/api/time>

## License

Licensed under the [MIT License](LICENSE.md).
