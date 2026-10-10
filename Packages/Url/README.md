# Url

URLs: parsing, normalizing and resolving them, and reading and building their queries.

## Installation

```sh
rux add Rux/Url
```

## What it provides

| Module      | Covers                                                                                                   |
| ----------- | -------------------------------------------------------------------------------------------------------- |
| `Url`       | `Url` with `Parse`, `Resolve`, component accessors, `SetPath`, `SetQuery` and `SetFragment`, and `HostKind` |
| `Query`     | `Pairs` for walking a query's `QueryPair`s, `Decode` for turning a component into text, and `QueryBuilder` |
| `Error`     | `UrlError`, why text is not a URL                                                                        |

The module names describe where each declaration comes from; they are not part of an import path.

## Example

```rux
import Allocator::SystemAllocator;
import Url::{ Pairs, Url, UrlError };

func SearchTerm(link: char8[..]) -> bool ! UrlError {
    let page = Url::Parse(SystemAllocator(), "https://example.com/docs/index.html")?;
    let target = page.Resolve(link)?;
    for pair in Pairs(target.Query() ?? "") {
        if pair.name.length == 1 && pair.name[0] == 'q' { return true; }
    }
    return false;
}
```

## Design

A URL is read by RFC 3986 and kept as one normalized ASCII text with the place of each component in it, so every accessor is a view and two URLs the RFC calls equivalent under syntax-based normalization are equal as text. Normalizing lower-cases the scheme and a registered host name, writes percent-encoding with upper-case hexadecimal and decodes the escapes of unreserved characters, removes dot segments from the path, leaves out a scheme's default port, and gives an `http`-like URL with an empty path the path `/`. An IPv4 host keeps its dotted form and an IPv6 host is written in the canonical form of RFC 5952. A character a component cannot hold as itself, such as a space or a non-ASCII byte, is percent-encoded rather than refused, as browsers do; a host is the exception, and refuses one, since escaping it would name a different machine. Internationalized host names are not converted to their ASCII form, so a non-ASCII host is refused.

`Resolve` follows section 5 of RFC 3986, and gives the results of its examples, except that a resolved `http` URL with an empty path is written with `/`. Components stay percent-encoded; `Decode` turns one into text, reading `+` as a space when asked to, as a query is read. `QueryBuilder` writes pairs with the encoding HTML forms use, ready for `SetQuery`.

## Documentation

<https://rux-lang.dev/docs/api/url>

## License

Licensed under the [MIT License](LICENSE.md).
