# The device — BUENA IP-615WDC

Addresses, MACs and serials are deliberately left out of this repo; they live in the site's own
private records.

| | |
|---|---|
| Device type | `Address Terminal` (IP- series) |
| Firmware | `TM35 V5.7.9` |
| Term ID / Name as shipped | `1` / `speaker 1` |
| Server IP as shipped | a `192.168.1.x` factory default — unreachable on any other network |
| Open ports | TCP **29751** only |
| Hardware | 15 W, 8 Ω, 91 dB, 90–16 000 Hz, 6″ woofer + 2″ tweeter, PoE **or** DC 12–24 V, one line-in, one alarm trigger |

Sold as a wall-mount network PA speaker; a typical listing is
<https://th.aliexpress.com/item/1005012080250093.html>. The pigtail carries an RJ45 and a DC
barrel jack side by side, which is the physical form of "PoE **or** DC 12–24 V" — useful to know
when a PoE budget is in question, because the unit will run just as happily off a DC adapter.

*(No product photo here: the listing images belong to the seller. A photo of an actual unit,
taken by whoever runs this, would be a welcome addition.)*

## Correction, 2026-08-20

This repo was started on the belief that the IP- series cannot register to a PBX. **It can.**
`Config Tool V5.7.5` has a *Settings* tab whose *Main Device Settings → SIP Settings* page
carries `Enable SIP Service`, `Connect SIP Server`, `Server-IP or Domain`, `Server Port`,
`SIP Account` and `Password`. The earlier conclusion came from reading only the tool's
*Search Device* grid, which has no SIP fields at all.

The bridge is therefore not the only way in — but it keeps its own value: one AudioSocket
target can page several terminals at once, and `TSDK_BroadMp3File` plus cron replaces the
vendor's Windows scheduler. Native SIP is the simpler route for a single speaker.

## The trap

BUENA sells the identical cabinet in two incompatible product lines:

| | SIP- series | IP- series (**this unit**) |
|---|---|---|
| Config tool | `parrot-man` (AoIP device manager) | `ConfigTool.exe` (TOOL_V575) |
| Discovery | UDP **7669** ("viper", 202-byte payload) | UDP **29762** (src 29772, 81–86 bytes) |
| Server | `parrot-server` | `TbsServer.exe` / this bridge |
| PBX | registers to a PBX directly, 2 SIP accounts | 1 SIP account — under *Settings → Main Device Settings → SIP Settings* |
| Device web admin | `:8080`, a fixed default password (2.5.x firmware) | none |
| Aux line-in | none | 1 × external audio input |

Every vendor manual says it outright: *"This two kinds of speakers connect different softwares
and not compatible."* Sellers list either model against the same photo.

🚫 **Never flash SIP- series firmware (`*.bau`) onto this device.** Different product line;
there is no documented recovery path and the only surviving port ignores every probe.

## How it was misidentified

Setup began with the SIP- line's `parrot-man`, which never found the speaker. Ruled out in
turn: VLAN (ARP resolves the MAC directly, ping TTL 64, one flat `/22`), Windows Firewall (off
on all profiles, datagrams seen on the wire), NIC selection (single adapter; the tool logs the
interface it picked).

A packet capture then showed `parrot-man` broadcasting correctly to UDP 7669, and a
byte-identical replay **from a host on the speaker's own subnet** drew zero replies; a unicast
to 7669 returned ICMP *port unreachable*, identical to an unused port. That reads as "the
application is dead" — and it was wrong. Nothing listened on 7669 because 7669 is the *other*
line's port.

The IP- line's `ConfigTool` found the speaker on the first click. Its `Change Settings` dialog
also explained the one clue that never fit the dead-app theory: a factory `Server IP` on a
network that does not exist here, which the speaker keeps ARPing the gateway to reach.

**The lesson: when a vendor ships one cabinet in two incompatible lines, confirm which line the
unit is before debugging its silence.** The label settles it in ten seconds.

## Diagnostic notes worth keeping

- A capture taken on a *third* host only sees broadcasts; unicast replies between the speaker
  and the tool's PC never cross it. "No reply in the capture" proves nothing unless the probe
  was sent from the capturing host.
- The device rate-limits ICMP hard — 34 replies to 65 535 UDP probes — so port-sweeping it to
  find a listener would take ~18 hours.
- `parrot-man` writes `C:\parrot\parrot-man\logs\parrot-man-YYYYMMDD.log`, which prints the
  datagram it sends and the interface it chose. Fastest diagnostic available, mentioned in no
  manual. (It also logs `ERROR: license too short` at every start — a separate matter, and moot
  once the product line was corrected.)
