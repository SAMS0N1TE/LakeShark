# LAN SCAN

Who is on the joined network. API 2.8, needs the NETWORK permission.

| Step | Through | Finds |
|---|---|---|
| Subnet | `lap_net_info` | address, mask, gateway; a mask wider than /24 is scanned as the /24 around the board |
| ARP sweep | `lap_arp_probe`, `lap_arp_table` | every host that is switched on, with its MAC |
| Ping sweep | `lap_ping_*`, four at a time, 300 ms each | round trips; hosts ARP found are pinged first |
| mDNS | `lap_mdns_browse` (`_http`, `_workstation`, `_ssh`, `_smb`, `_googlecast`, `_ipp`) | host names |
| SSDP | `M-SEARCH` to 239.255.255.250:1900 with `lap_udp_*` | the `SERVER:` line of UPnP devices |
| Ports | `lap_tcp_connect` to 30 common ports, three at a time, 60 ms apart, 1.5 s timeout | open, closed (refused) or filtered (no answer) |

MAC vendors come from a small built-in OUI list (Raspberry Pi, Espressif,
TP-Link, Ubiquiti, Apple, Google, Synology, Sonos, Hue, virtual machines and
a few more). A locally administered MAC shows as `private MAC`.

Boards built without mDNS (the T-Display OTA profile) show `mDNS n/a`; the
rest of the scan still runs.

## Screens

- Handheld: two rows per host (address, round trip, vendor; then MAC and
  name). Tap a host to select it, tap it again for its details.
- Wide (90 columns or more): one row per host with MAC, vendor, round trip,
  name and open ports.
- Host details: everything known about it and the port scan table.

## Keys and buttons

| Key | Button | Does |
|---|---|---|
| S | SCAN / STOP | scan again (the list is kept and updated) or stop |
| H, ENTER | HOST | details of the selected host |
| P | PORTS | TCP port scan of the selected host |
| W | SAVE | CSV to `apps_data/lan_scan/scan_<date>_<time>.csv` |
| UP, DOWN, swipe | | select |
| BACK | BACK | back to the list |

CSV columns: `ip,mac,vendor,rtt_ms,name,ssdp_server,found_by,open_ports`.

## Simulator

lssim answers ARP, ping and the interface from a fixed LAN (192.0.2.0/24, see
`docs/NETDIAG.md`), so the list is the same on every run. The SSDP search and
the port scan use real host sockets; add `1:udpdrop=1` to `LAPSIM_SCRIPT` to
keep the M-SEARCH off your network.
