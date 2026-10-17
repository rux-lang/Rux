# Net

Networking: IPv4 and IPv6 addresses, TCP listeners and streams, UDP sockets, and host name resolution.

## Installation

```sh
rux add Rux/Net
```

## What it provides

| Module    | Covers                                                                                                         |
| --------- | -------------------------------------------------------------------------------------------------------------- |
| `Address` | `Ipv4Address`, `Ipv6Address`, `IpAddress` and `SocketAddress`, each with `Parse`, `WriteText`, `Equals` and `Display` |
| `Tcp`     | `TcpListener` with `Bind`, `Accept` and `LocalAddress`; `TcpStream`, an `Io` reader and writer, with `Connect`, `Shutdown`, `SetNoDelay` and timeouts; `SetNonBlocking` and `PollSource` on both |
| `Udp`     | `UdpSocket` with `Bind`, `SendTo`, `ReceiveFrom`, `Connect`, `Send`, `Receive`, a read timeout, `SetNonBlocking` and `PollSource`; `Datagram` |
| `Resolve` | `Resolve`, the addresses of a host name                                                                         |
| `Dns`     | `Resolver`, a DNS client with `WithServers`, `FromSystem`, `SetTimeout`, `SetAttempts` and `Lookup`              |
| `Poll`    | `Poller` with `Register`, `Modify`, `Deregister` and `Wait`; `Interest`, `PollSource` and `PollEvent`              |

The module names describe where each declaration comes from; they are not part of an import path.

## Example

```rux
import Allocator::SystemAllocator;
import Io::{ IoError, ReadExact, WriteAll };
import Net::{ Resolve, TcpStream };

func Greet(host: char8[..]) -> ! IoError {
    let addresses = Resolve(SystemAllocator(), host, 7)?;
    var stream = TcpStream::Connect(addresses.AsSlice()[0])?;
    WriteAll(stream, "hello")?;
    var echoed: char8[5] = ['\0'; 5];
    ReadExact(stream, echoed[..])?;
}
```

## Design

Sockets block by default: a call waits until it can do its work, or until the timeout set for it runs out and it fails with `TimedOut`. `SetNonBlocking` makes a socket's calls fail with `WouldBlock` instead whenever they would wait, and a `Poller` says when they would not: sockets are registered with a token and the readiness wanted, and `Wait` reports the ready ones by token, waiting up to a timeout. It is level-triggered, so a socket is reported for as long as it is ready, and runs on epoll on Linux, kqueue on FreeBSD and macOS, and `WSAPoll` on Windows. A stream a listener accepts starts blocking whatever the listener is. Failures are `Io::IoError` values, so a `TcpStream` passes straight to anything that takes an `Io` reader or writer, and the reasons a connection fails — refused, reset, aborted, unreachable, an address in use — have kinds of their own. Writing to a connection the peer has closed fails with `BrokenPipe` rather than ending the process with a signal. Every socket is closed when its value is dropped, and none is inherited by a child process.

Addresses are plain values. IPv4 addresses are read in the four-part decimal form only, without the leading zeros some systems read as octal; IPv6 addresses take the `::` shorthand and an embedded IPv4 tail, and are written as RFC 5952 asks, in lower case with the longest run of zeros shortened and an IPv4-mapped address with its IPv4 tail, as in `::ffff:192.0.2.1`. A socket address writes an IPv6 address in brackets, as in `[::1]:8080`. Every address type also renders through `{}`, padded to the placeholder's width, and answers `IsLoopback`, `IsUnspecified`, `IsPrivate` (RFC 1918, or a unique local IPv6 address), `IsLinkLocal` and `IsMulticast`.

On Linux and FreeBSD the sockets are system calls. On macOS they come from libSystem and on Windows from Winsock, which is started the first time a socket is opened. On FreeBSD, macOS and Windows `Resolve` asks the system's resolver, `getaddrinfo`, so names resolve as they do for every other program on the machine. On Linux that resolver belongs to the C library, so `Resolve` reads `/etc/hosts` and asks the name servers in `/etc/resolv.conf` itself, and a program that resolves names stays a static binary; search domains and the name service switch are not followed. `Resolver` is that DNS client, usable on every system with servers of the caller's choosing: it asks each server in turn for the IPv4 and then the IPv6 addresses over UDP, and over TCP when an answer is too long for a datagram. A numeric host is read directly.

## Documentation

<https://rux-lang.dev/docs/api/net>

## License

Licensed under the [MIT License](LICENSE.md).
