# Build and deploy

Build on the code machine, run on the PBX. The two are not the same Debian, which is the one
thing that will bite you.

| Host | Role | Debian | Notes |
|---|---|---|---|
| build box | build | **13.4**, gcc 14.2 | has Docker |
| PBX box | run | **12.13** | Asterisk 20.19.0, with `app_audiosocket.so` / `chan_audiosocket.so` / `res_audiosocket.so` present |

## Why not just `gcc` on the build host

The build host runs Debian 13 (glibc 2.41); the PBX runs Debian 12 (glibc 2.36). glibc is
backward-compatible, not forward-compatible, so a binary built on 13 refuses to start on 12.
Build inside a Debian 12 container instead — same box, right libc:

```bash
ssh <build-host>
mkdir -p /tmp/cc12
printf 'FROM debian:12\nRUN apt-get update -qq && apt-get install -y -qq build-essential file\n' > /tmp/cc12/Dockerfile
docker build -t deb12-cc /tmp/cc12          # once
```

## Build

```bash
# on the build host, with the repo (including vendor/sdk) at /tmp/pabuild
docker run --rm -v /tmp/pabuild:/src -w /src/src deb12-cc make
file pa-bridge   # ELF 64-bit LSB executable, x86-64, dynamically linked
```

Two link-time facts, both already handled in the Makefile:

- **`libCtsSdk.a` is not PIE**, so the final link needs `-no-pie`. Without it:
  *"relocation R_X86_64_32S against `.rodata` can not be used when making a PIE object"*.
- **The vendor headers expect the caller to define `WINAPI` / `CALLBACK` and include
  `win_types.h` first** — exactly as their own demo does. Include `cts_sdk.h` alone and every
  declaration silently vanishes behind implicit-declaration warnings.

Linking `libCtsSdk.a` statically is deliberate: the result is one file to copy to the PBX, with
no vendor `.so` to install or version.

## Deploy

```bash
scp /tmp/pabuild/src/pa-bridge <pbx-host>:/usr/local/bin/pa-bridge
scp deploy/pa-volume           <pbx-host>:/usr/local/bin/pa-volume   # needs nc on the target
scp deploy/pa-bridge.service   <pbx-host>:/etc/systemd/system/
ssh <pbx-host> 'chmod 755 /usr/local/bin/pa-volume &&
                systemctl daemon-reload &&
                systemctl enable --now pa-bridge &&
                systemctl status pa-bridge'
```

That is the complete on-target footprint: **two files in `/usr/local/bin`, one unit file, and
one firewall rule**. Nothing else is installed and nothing is left in `/tmp`.

Edit `ExecStart` first: the argument is the comma-separated list of terminal IDs (the `Term ID`
column in `ConfigTool`), then the AudioSocket port.

## Point the speaker at the bridge

In `ConfigTool.exe` (Windows): *Search Device* → tick the speaker → **Change Settings** →
`Server IP` = the host running pa-bridge → **Save** → **Reboot Device**.

Leave `Auto Getting IP Address` ticked — the speaker's own address is a working DHCP lease.

Until this is done, the bridge starts fine but `TSDK_SetupGroup` returns
`-802 CERR_INVALID_PARAMETER`: no terminal has connected, so there is nothing to put in a group.

## Asterisk side

AudioSocket ships with Asterisk 20, so the page target is one dialplan line:

```
exten => _X.,n,Dial(AudioSocket/<bridge-host>:9092/${UUID},30)
```

and joining the existing house page is the usual astdb entry:

```bash
asterisk -rx 'database put pagelist <ext> AudioSocket/<bridge-host>:9092/00000000-0000-0000-0000-000000000001'
```

⚠️ `pagelist` values are dialled by `Page()` via `${PAGELIST}`; confirm the channel string form
against `[page-helpers]` in `extensions.conf` before relying on it.

## Firewall / ports

| Port | Who connects | Expose it? |
|---|---|---|
| `29783/tcp` | the speaker, registering to its "Center Server" | **yes, to the LAN only** — this is the port the firmware dials |
| `9092/tcp` | Asterisk's `AudioSocket()` application | no — loopback is enough when the bridge runs on the PBX |
| `9093/tcp` | `pa-volume` | no — the bridge binds it to `127.0.0.1` and it cannot be exposed |
| `29883/tcp` | the SDK's own listener, fed by the bridge over loopback | no |

**Resolved:** the SDK's listener is a compiled-in 29883 while the firmware dials 29783. Rather
than requiring an `iptables` redirect on every host, the bridge listens on 29783 itself and
pumps bytes to the SDK. Nothing external is needed.

On a host with a default-deny firewall, open exactly one port. On UFW:

```bash
ufw allow from <lan>/22 to any port 29783 proto tcp comment 'PA speaker -> pa-bridge'
```

⚠️ A default-deny firewall is a *silent* failure here: the speaker retries forever and the
vendor's config tool reports only `Connect timeout`, which looks identical to "the service is
not running". Check with `nc -z <host> 29783` from another machine before believing anything
else.

## Pointing a speaker at the bridge

From the vendor's Windows `Config Tool` → *Settings* → *Center Server* → set `Server IP` →
*Save* → reboot the device.

🔑 **The bridge must already be listening at that address before you press Save.** The tool
verifies the centre server first and abandons the entire write if it cannot connect — the
device keeps its old server and nothing on screen says so. Start the service, confirm the port
is reachable, then save.

Verify from the device's own behaviour rather than the tool: it ARPs for its configured server
about once a second (see [`operations.md`](operations.md)).

## Day-to-day operation

Volume, the control port, the health check and a symptom table live in
[`operations.md`](operations.md).
