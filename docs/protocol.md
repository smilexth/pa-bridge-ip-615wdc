# What the wire looks like

Observations, not vendor documentation. Everything here was measured with `tcpdump` against
hardware we own, because none of it is written down anywhere the vendor publishes. It is
recorded so the next person does not have to repeat the measurements.

## Ports

| Port | Direction | What it is |
|---|---|---|
| **UDP 29762** | config tool → broadcast | Device discovery for the **IP-** series. Source port 29772, 81–86 byte payloads, sent to `255.255.255.255` only. The tool also fires SSDP at `239.255.255.250:1900` and an 8-byte probe on UDP 5678 |
| **TCP 29783** | terminal → server | Where an IP- series terminal registers. Also what the config tool connects to when verifying a "Center Server" before saving |
| **TCP 29883** | — | Where `libCtsSdk` (the x86-64 V308 build) puts its own listener. Compiled in, no API to change it, and not even present as a string in the library |
| **TCP 29751** | — | Open on the terminal itself. Accepts a connection and then waits for the client; it ignores the discovery framing, a header-only probe and a bare newline |
| **UDP 7669** | — | Not this product line. It is the **SIP-** series' discovery port ("viper", 202-byte payload). An IP- series terminal answers `ICMP port unreachable`, exactly like an unused port |

**The 29783/29883 gap is the practical problem.** The firmware dials 29783; the SDK listens on
29883. Since the SDK's port cannot be moved, this bridge accepts 29783 itself and relays to the
SDK over loopback.

## Terminal IDs

`dwTermID` is a 32-bit value: **`zone << 16 | device`**, where both come from the config tool
(zone 1–10, device 1–600). The tool's display shows only the device number, so its "Term ID 1"
is `0x00010001` = **65537**. Group calls fail with `-802 CERR_INVALID_PARAMETER` for anything
else, so read the ID from the SDK's connect callback rather than transcribing it.

## Behaviours worth knowing

**A terminal ARPs for its configured server about once a second.** This is the cheapest way to
read back what a device actually stored — no tool, no login, no interruption:

```bash
tcpdump -nn -i <iface> 'arp and ether host <speaker-mac>'
# who-has 10.0.0.5 tell 10.0.0.9    <- that is its Center Server
```

It is also how a wrong address gets caught: a speaker will faithfully call an address that
belonged to somebody's laptop an hour earlier, forever, without complaining.

**The config tool verifies before it writes.** Saving a Center Server address opens a TCP
connection to port 29783 *at the address being saved*. If nothing answers, it reports
`Center-Server updates failed, Error-Info: Connect timeout` and **abandons the entire write** —
the device silently keeps its old value. Start the server first, prove the port with
`nc -z <host> 29783`, then save.

**The terminal rate-limits ICMP hard.** 65535 UDP probes drew 34 `port unreachable` replies, so
a port sweep is not a viable way to find its listeners — it would take about 18 hours. Probing
one port at a time works fine.

**Registration is inbound.** The terminal is the client: it connects out to its Center Server and
keeps reconnecting. Nothing needs to reach *into* the speaker, which is why the bridge is a
server rather than a client, and why the only firewall rule needed is inbound 29783.

## AudioSocket, on the Asterisk side

Framing is Asterisk's, documented at
<https://docs.asterisk.org/Configuration/Channel-Drivers/AudioSocket/>: one byte of type, two
bytes of big-endian length, then the payload. Types used here are `0x01` (UUID, 16 bytes),
`0x10` (audio), `0x00` (terminate) and `0xff` (error).

Two measured details that the documentation does not put in one place:

- **Frame size follows the channel.** A softphone call arrives as **160-byte** payloads (10 ms of
  8 kHz 16-bit mono); file playback gives 320. The SDK only accepts multiples of 320, so the
  bridge re-frames.
- **The channel driver is not the application.** `Dial(AudioSocket/…)` hands the socket whatever
  codec the caller negotiated — a ulaw call arrives as 8-bit companded bytes. The
  `AudioSocket()` application always sends 16-bit 8 kHz PCM. If audio comes out as hiss, this is
  why; capture loopback and look at the payload rather than theorising:

```bash
tcpdump -i lo -s 0 -w /tmp/as.pcap 'tcp port 9092'
# payload starting fa fa fa fa … with byte values clustered high is ulaw silence, not PCM
```

## Method note

Captures taken on a *third* host only show broadcast traffic — unicast between two other
machines never crosses it. Probe from the capturing host, or capture on one of the endpoints,
or the absence of packets means nothing.
