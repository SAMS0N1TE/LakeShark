# THROUGHPUT

An iperf2-compatible client. On a computer on the same network run

    iperf -s          # TCP, port 5001
    iperf -s -u       # UDP

then set SERVER to its address (`host` or `host:port`) and START. iperf3 uses
a different protocol and does not answer.

- TCP: connects, sends iperf2's 24-byte client header with no flags (a plain
  test to any iperf2 server) and streams for 5, 10, 30 or 60 s.
- UDP: datagrams of 1470 bytes in iperf2's format (sequence number, send
  time), paced at 1, 2, 5, 10 or 20 Mbit/s. At the end it sends the negative
  sequence number iperf2 uses to stop and shows the server's report: bytes,
  lost datagrams, out of order and jitter.

The graph shows Mbit/s every half second. What is measured is the app SDK's
socket path: bytes the board's network worker accepted (as iperf2's client
reports bytes written). The worker's queues are small (4 KB for TCP, four
datagrams for UDP), so expect a few Mbit/s from an app, less than the board's
own Wi-Fi link can carry. A name is looked up with `lap_dns_resolve` (API 2.8)
before a UDP test. Settings are kept in `apps_data/throughput/settings.txt`.

| Key | Button | Does |
|---|---|---|
| S, SPACE, ENTER | START / STOP | run or stop |
| E | SERVER | type the server (or tap the Server line) |
| U | TCP / UDP | switch protocol |
| T | TIME | 5, 10, 30, 60 s |
| R | RATE | UDP rate |

## Simulator

lap_tcp and lap_udp are real host sockets in lssim, so a local `iperf -s`
works. `app-sdk/tests/test_netdiag28.py` uses small Python stand-ins for the
TCP sink and the UDP server report.
