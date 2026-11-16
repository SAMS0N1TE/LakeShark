# NET DIAG

Ping, traceroute, DNS lookup, Wake-on-LAN and interface info, one tab each.
API 2.8, needs the NETWORK permission.

| Tab | Does |
|---|---|
| PING | one echo request a second to the host (the gateway by default): sent, received, loss, min/avg/max and a round-trip graph; timeouts show as a red x |
| TRACE | one ping per hop with TTL 1, 2, 3 ... (three probes each, four hops in flight) until the destination answers; up to 30 hops |
| DNS | `lap_dns_resolve` of the name; the last ten answers with the time each took |
| WOL | magic packet (6 x FF, the MAC 16 times) as a UDP broadcast to port 9 of the subnet's broadcast address; `To` takes `address` or `address:port` |
| INFO | Wi-Fi network and signal, address, mask, gateway, DNS server, MAC, Ethernet (not on this firmware) and the board's ARP table |

## Keys and buttons

| Key | Button | Does |
|---|---|---|
| 1-5, TAB, LEFT/RIGHT, swipe | tab row | change tab |
| S, SPACE, ENTER | START / LOOKUP / SEND / REFRESH | the tab's action |
| E, H | HOST / NAME / MAC | type the target (or tap the target line) |
| C | CLEAR / TO | clear the results; on WOL, the broadcast address |

## Simulator

The fixed LAN of `docs/NETDIAG.md`: 192.0.2.1, .3, .10, .23 and .42 answer,
anything outside the LAN is four hops away, `*.invalid` does not resolve.
`LAPSIM_SCRIPT` `netdown=1` and `dnsfail=1` inject failures. The WoL packet
is a real UDP datagram from the host.
