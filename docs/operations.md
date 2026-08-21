# Running it

## Volume

The bridge sets the speaker's own amplifier level — not a software gain, so nothing is lost to
digital attenuation.

```bash
pa-volume          # terminals=1 volume=60
pa-volume 40       # quieter
pa-volume 100      # maximum
pa-volume 0        # mute
pa-volume get      # ask the speaker what it is really set to; the answer lands in the log
```

`pa-volume` is a three-line shell wrapper around the control port:

```sh
printf 'vol 40\n' | nc 127.0.0.1 9093
```

The hardware has **10 steps**, so the 0–100 you type is scaled to 0–10 — 44 and 40 do the same
thing. The level is re-applied on **every terminal connect**, so a speaker reboot, a service
restart or a network blip can never leave it at some forgotten setting. The startup default
comes from `PA_BRIDGE_VOLUME` in the unit file.

⚠️ **Never switch this to `TSDK_Req_SetAudLevelEx()`**, the 0–100 "Ex" call. It returns
`CERR_SUCCESS` and then does the wrong thing on TM35 / BIOS 5.7.9: value 100 became level 10 and
was deafening, while 60, 40 and 30 all collapsed to level 1 and were inaudible. There is no
usable range in between. The coarse `TSDK_Req_SetAudLevel()` behaves, and the firmware reports
sane readings back (level 6 reads back as internal value 85).

## Control port

`127.0.0.1:9093`, one line in and one line out, **bound to loopback** so it cannot be reached
off the box:

| Command | Effect |
|---|---|
| `vol <0-100>` | set the level on every known terminal, and remember it for reconnects |
| `get` | ask each terminal its current level; answers appear in the log |
| `status` | `terminals=N volume=V` |

## Health check

```bash
systemctl status pa-bridge
ss -lnt | grep -E ':9092|:29783|:9093'    # three listeners
pa-volume                                  # terminals=N tells you a speaker is attached
journalctl -u pa-bridge -n 20 --no-pager
```

A healthy startup, then a speaker attaching:

```
[vol ] speaker volume will be set to 60/100 on connect
[as ] AudioSocket listening on :9092
[shim] listening :29783 -> 127.0.0.1:29883
[ctl ] listening on 127.0.0.1:9093 (vol N | get | status)
[shim] <speaker-ip> connected
[sdk] connected: id=65537 ip=127.0.0.1 name=speaker1
[vol] requested 60/100 = amp level 6 on terminal 65537
[vol] terminal 65537 set -> result 0 (value 85, level 6)
```

Two log lines look wrong and are not: `id=65537` is `zone<<16 | device` for zone 1 / device 1,
and `ip=127.0.0.1` is what the SDK sees because the terminal reaches it through the bridge's own
loopback shim.

`[sdk] terminal state changed` appears constantly, including during playback. It is noise.

## Troubleshooting by symptom

| Symptom | What it has actually meant |
|---|---|
| Call connects, complete silence | Either the group was never built (`SetupGroup … -> -802`, wrong terminal ID) or the level is at 1 — inaudible across a room even though the SDK reports success |
| Hiss, speech unrecognisable | Something other than 8 kHz PCM is arriving. Almost always `Dial(AudioSocket/…)` instead of the `AudioSocket()` application: a ulaw caller's companded bytes read as 16-bit PCM |
| `… not a multiple of 320, skipped` in the log | An old build. Current code re-frames; if you see this, the running binary predates that fix |
| `StartBroadVoice -> -916` | `CERR_GROUP_NOT_SETUP` — the group setup failed first; look at the `SetupGroup` line just above it |
| No terminal ever connects | The speaker is pointed somewhere else, or a firewall drops 29783. On a UFW box remember `INPUT policy DROP` blocks it silently |
| Config tool's *Save* fails with `Connect timeout` | Nothing is listening on 29783 **at the address being saved**. The tool verifies before writing and abandons the whole write, leaving the old value in place |

## Reading back what a speaker really has

Independent of any tool, and free: a terminal ARPs for its configured server about once a
second. Capture ARP on a host that shares the segment:

```bash
tcpdump -nn -i <iface> 'arp and ether host <speaker-mac>'
# who-has <server-ip> tell <speaker-ip>
```

That is how a mistyped server address was caught: the speaker was faithfully calling an address
that had been a laptop's temporary IP an hour earlier.

## Capturing what the bridge receives

When audio is wrong rather than absent, capture the AudioSocket stream on loopback and inspect
the bytes rather than guessing at codecs:

```bash
tcpdump -i lo -s 0 -w /tmp/as.pcap 'tcp port 9092'
```

Strip the 3-byte AudioSocket headers (`type`, 16-bit big-endian length) and look at the payload.
A stream that starts `fa fa fa fa …` with byte values clustered high is ulaw silence, not PCM —
that single observation is what identified the hiss.
