# TimeZone

Time zones: the tz database, POSIX rules, the local zone, and local times that happen twice or never.

## Installation

```sh
rux add Rux/TimeZone
```

## What it provides

| Module    | Covers                                                                                                |
| --------- | ----------------------------------------------------------------------------------------------------- |
| `Zone`    | `TimeZone` with `FromTzif`, `FromRule`, `Utc`, `StateAt`, `OffsetAt`, `ToLocal` and `Resolve`; `ZoneState` and `LocalTime` |
| `System`  | `TimeZone::Load` by tz database name, and `TimeZone::Local`                                            |
| `Error`   | `TimeZoneError`                                                                                        |

The module names describe where each declaration comes from; they are not part of an import path.

## Example

```rux
import Allocator::SystemAllocator;
import Time::Timestamp;
import TimeZone::{ TimeZone, TimeZoneError };

func KyivOffset() -> int32 ! TimeZoneError {
    let zone = TimeZone::Load(SystemAllocator(), "Europe/Kyiv")?;
    return zone.OffsetAt(Timestamp::Now()).seconds;
}
```

## Design

A zone is the tz database's history of the offsets it kept and when they changed, read from a TZif file as RFC 8536 describes it, followed by the POSIX `TZ` rule it keeps after its last listed change; or a rule alone. Versions 1 to 4 of TZif are read, using the 64-bit history and the footer rule from version 2 on; leap-second records are skipped, so the `right/` zones read as their `posix/` twins. Rules take quoted abbreviations such as `<+0330>`, offsets with minutes and seconds, the `Jn`, `n` and `Mm.w.d` dates and the extended change times of TZif version 3, and work south of the equator, where daylight time spans the new year.

`StateAt` gives a moment's offset, whether it is daylight time, and its abbreviation; `ToLocal` gives its local date and time. `Resolve` goes the other way, and says whether a local date and time happened once, twice as clocks fell back, or never as they sprang forward, with both candidate moments.

`Load` looks a zone up by name in the directory `TZDIR` names, or else in the directories the Unix systems keep the database in. Names that leave the directory are refused. Windows keeps no such directory, so there a zone loads only from a `TZDIR` the program provides. `Local` reads `TZ` or `/etc/localtime` on the Unix systems; on Windows it reads the system's zone as rules for standard and daylight time, which describe current practice rather than the zone's history.

## Documentation

<https://rux-lang.dev/docs/api/timezone>

## License

Licensed under the [MIT License](LICENSE.md).
