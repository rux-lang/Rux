# Net

Networking: IPv4 and IPv6 addresses, TCP listeners and streams, UDP sockets, and host name resolution.

## Installation

```sh
rux add Rux/Net
```

## What it provides

| Module    | Covers                                                                                                         |
| --------- | -------------------------------------------------------------------------------------------------------------- |
| `Address` | `Ipv4Address`, `Ipv6Address`, `IpAddress` and `SocketAddress`, each with `Parse`, `WriteText` and `Equals`        |
| `Tcp`     | `TcpListener` with `Bind`, `Accept` and `LocalAddress`; `TcpStream`, an `Io` reader and writer, with `Connect`, `Shutdown`, `SetNoDelay` and timeouts |
| `Udp`     | `UdpSocket` with `Bind`, `SendTo`, `ReceiveFrom`, `Connect`, `Send`, `Receive` and a read timeout; `Datagram`       |
| `Resolve` | `Resolve`, the addresses of a host name                                                                         |

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

Sockets block: a call waits until it can do its work, or until the timeout set for it runs out and it fails with `TimedOut`. Failures are `Io::IoError` values, so a `TcpStream` passes straight to anything that takes an `Io` reader or writer, and the reasons a connection fails — refused, reset, aborted, unreachable, an address in use — have kinds of their own. Writing to a connection the peer has closed fails with `BrokenPipe` rather than ending the process with a signal. Every socket is closed when its value is dropped, and none is inherited by a child process.

Addresses are plain values. IPv4 addresses are read in the four-part decimal form only, without the leading zeros some systems read as octal; IPv6 addresses take the `::` shorthand and an embedded IPv4 tail, and are written as RFC 5952 asks, in lower case with the longest run of zeros shortened. A socket address writes an IPv6 address in brackets, as in `[::1]:8080`.

On Linux and FreeBSD the sockets are system calls. On macOS they come from libSystem and on Windows from Winsock, which is started the first time a socket is opened. `Resolve` asks the system's resolver, `getaddrinfo`, so names resolve as they do for every other program on the machine; on the Unix systems that resolver belongs to the C library, so a program that resolves names is linked against it. A numeric host is read directly.

## Documentation

<https://rux-lang.dev/docs/api/net>

## License

Licensed under the [MIT License](LICENSE.md).
