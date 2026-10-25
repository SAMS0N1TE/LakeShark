/* The board's name on the local network: just enough multicast DNS
   (RFC 6762) for <name>.local to find it, so that the web screen saved to
   a phone's home screen keeps working when the router hands out another
   address. ls_rterm.c owns the socket; this is the packets only, no
   sockets, so the bench runs it on the PC.

   Only A records for the one name, answered and announced; no probing for
   a name already taken, no services. Two boards on one network both
   answer, and the browser goes to whichever it heard first. */

#ifndef LS_MDNS_H
#define LS_MDNS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_MDNS_PORT  5353
#define LS_MDNS_GROUP 0xE00000FBu       /* 224.0.0.251, host order */

/* The answer to a query in pkt that asks for host.local's address (ip in
   network order), or 0 when it does not ask, or is not a query. legacy:
   it came from a port other than 5353, so the answer goes back to it alone
   with the query's id and question, as a plain DNS answer. *unicast: the
   asker wants it straight back (a QU question), not to the group. */
size_t ls_mdns_answer(const uint8_t *pkt, size_t n, const char *host, uint32_t ip, bool legacy,
                      uint8_t *out, size_t cap, bool *unicast);

/* An unsolicited answer, to send to the group when the name or the address
   is new, so that caches that had the old one let it go. */
size_t ls_mdns_announce(const char *host, uint32_t ip, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
