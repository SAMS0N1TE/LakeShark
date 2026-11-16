# Network diagnostics (API 2.8)

Ping, traceroute, DNS lookups, ARP and the interface's own addresses, for
LAN tools. Declared in `include/lakeshark_app.h` under `LAP_API_28`.
Examples: `apps/lan_scan`, `apps/net_diag`, `apps/throughput`.

| Call | Does |
|---|---|
| `lap_net_info(iface, &info)` | address, mask, gateway, DNS server, MAC, link state, kind |
| `lap_ping_start(host, &cfg)` | starts `count` ICMP echo requests; returns a handle |
| `lap_ping_next(h, &reply)` | one answer per request: reply, TTL exceeded, unreachable or timeout |
| `lap_ping_stop(h)` | frees the handle |
| `lap_dns_resolve(name, timeout_ms)` | starts an A lookup; returns a handle |
| `lap_dns_result(h, ip)` | the address once it is known; a final answer frees the handle |
| `lap_arp_probe(ip)` | queues one ARP request on the active interface |
| `lap_arp_table(out, max)` | the IP/MAC pairs the board has seen, lowest address first |

## Rules

- The manifest declares `LAP_CAP_NETWORK`, as for `lap_tcp_*`. Without it
  every call except `lap_ping_stop` returns `LAP_IO_EACCES`.
- Every call returns at once. The LAP worker sends, waits and parses. Poll
  from `draw()`.
- IPv4 only. Hosts are dotted quads or names of up to 253 characters.
- Limits per app: four pings, four lookups, sixteen queued ARP requests.
  `LAP_IO_EBUSY` means all are in use.
- A ping sends a request only while fewer than eight answers wait unread,
  so an app that stops reading loses nothing; the run resumes when it reads.
- Closing the app stops its pings and frees every handle.
- Errors: `LAP_IO_ENOTFOUND` (the name did not resolve), `LAP_IO_ENETDOWN`
  (no interface up with an address), `LAP_IO_ETIMEOUT` (lookup timed out).

## Traceroute

Start a one-request ping per hop with `cfg.ttl` = 1, 2, 3 ... Each router on
the way answers `LAP_PING_TTL_EXCEEDED` with its address in `from`; the
destination answers `LAP_PING_REPLY`. Four pings may run at once, so an app
can probe several hops in parallel.

```c
lap_ping_config_t c = { .size = sizeof c, .version = 1, .count = 1, .ttl = hop, .timeout_ms = 2000 };
int h = lap_ping_start("example.org", &c);
/* in draw(): */
lap_ping_reply_t r;
if (lap_ping_next(h, &r) == 1)
    show(hop, r.type, r.from, r.rtt_us);
```

## ARP

The stack's ARP table holds only ten pairs. The board copies it twice a
second while an app uses these calls (ten times a second for two seconds after
a probe) and keeps the last 32 pairs it saw. A sweep that sends a few probes
a frame, as `apps/lan_scan` does, collects every MAC. Pairs expire
five minutes after they leave the stack's table.

## On the board

ICMP goes through one lwIP raw PCB in the tcpip thread. Requests are sent and
answers are timestamped there, so round trips are measured in microseconds.
No socket descriptor is used: apps keep their six TCP and four UDP sockets.
ESP-IDF's `ping_sock` was not used because it drops ICMP time-exceeded
messages, which traceroute needs, and it starts a task per session.

`LAP_NETIF_ETH` and `LAP_CAPTURE_ETH` are reserved for USB Ethernet. This
firmware has no Ethernet interface: `lap_net_info(LAP_NETIF_ETH, ...)`
returns `LAP_IO_ENOTSUP` with `kind` = `LAP_NETIF_NONE`.

## Simulator

lssim has no network for these calls. It answers from a fixed LAN:

| Address | Answers as |
|---|---|
| 192.0.2.2 | the board (Wi-Fi, MAC 02:4c:53:00:00:02) |
| 192.0.2.1 | gateway and DNS server (TP-Link OUI) |
| 192.0.2.3 | `sim.local`, the mDNS fixture (Espressif OUI) |
| 192.0.2.10 | Raspberry Pi OUI |
| 192.0.2.23 | Philips Hue OUI |
| 192.0.2.42 | Synology OUI |
| other 192.0.2.x | silent: requests time out |
| outside the LAN | four hops: 192.0.2.1, 203.0.113.1, 203.0.113.9, then the host |

Names: `localhost` is 127.0.0.1, `gateway.lan` and `router.lan` are
192.0.2.1, `sim.local` is 192.0.2.3, names ending `.invalid` fail, any other
name maps to a fixed 198.51.100.x. Round trips are fixed per address, so
renders and tests repeat exactly. `LAPSIM_SCRIPT` steps `netdown=1` (link
down) and `dnsfail=1` (every lookup fails) inject failures.

TCP and UDP (`lap_tcp_*`, `lap_udp_*`) stay real host sockets in lssim, so
the example apps' port scan, SSDP search and throughput test talk to the
machine running the simulator.
