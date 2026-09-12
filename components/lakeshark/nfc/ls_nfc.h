/* Public LakeShark NFC API. */

#ifndef LS_NFC_H
#define LS_NFC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Tag identity ------------------------------------------------------- */

typedef enum {
    LS_NFC_TECH_UNKNOWN = 0,
    LS_NFC_TECH_NFCA    = 1,
    LS_NFC_TECH_NFCB    = 2,
    LS_NFC_TECH_NFCF    = 3,
    LS_NFC_TECH_NFCV    = 4,
} ls_nfc_tech_t;

typedef enum {
    LS_NFC_TAG_UNKNOWN = 0,
    LS_NFC_TAG_TYPE2   = 1,   /* NFC Forum Type 2 (NTAG/UL) */
} ls_nfc_tag_type_t;

/* ISO/IEC 14443-A: UID is 4, 7 or 10 bytes.  atqa/sak are the raw values
   the PICC returned - kept for identification and later diagnosis. */
#define LS_NFC_UID_MAX 10

typedef struct {
    ls_nfc_tech_t     tech;
    ls_nfc_tag_type_t tag_type;
    uint8_t           uid[LS_NFC_UID_MAX];
    uint8_t           uid_len;      /* 4, 7 or 10 */
    uint16_t          atqa;         /* NFC-A ATQA, big-endian on wire */
    uint8_t           sak;          /* final SEL_RES / SAK */
} ls_nfc_tag_t;

/* --- NDEF records ------------------------------------------------------- */

typedef enum {
    LS_NFC_TNF_EMPTY        = 0x00,
    LS_NFC_TNF_WELL_KNOWN   = 0x01,
    LS_NFC_TNF_MIME         = 0x02,
    LS_NFC_TNF_URI          = 0x03,
    LS_NFC_TNF_EXTERNAL     = 0x04,
    LS_NFC_TNF_UNKNOWN      = 0x05,
    LS_NFC_TNF_UNCHANGED    = 0x06,
    LS_NFC_TNF_RESERVED     = 0x07,
} ls_nfc_tnf_t;

/* Records are views into the message buffer.  Nothing here is owned; the
   pointers stay valid for the lifetime of the message buffer passed to
   ls_ndef_parse (or, for events, until the next event fires). */
typedef struct {
    ls_nfc_tnf_t   tnf;
    bool           mb;              /* message-begin */
    bool           me;              /* message-end */
    bool           cf;              /* chunk-flag: this parser rejects it */
    bool           sr;              /* short-record (1-byte payload length) */
    bool           il;              /* id-length present */
    uint8_t        type_len;
    uint8_t        id_len;
    uint32_t       payload_len;
    const uint8_t *type;
    const uint8_t *id;
    const uint8_t *payload;
} ls_nfc_ndef_record_t;

/* --- Read result -------------------------------------------------------- */

typedef enum {
    LS_NFC_ERR_NONE               = 0,
    LS_NFC_ERR_TRANSPORT          = 1,   /* SPI / IRQ / reader-side fault */
    LS_NFC_ERR_TIMEOUT            = 2,
    LS_NFC_ERR_UNSUPPORTED_TECH   = 3,
    LS_NFC_ERR_UNSUPPORTED_TAG    = 4,
    LS_NFC_ERR_MALFORMED          = 5,
    LS_NFC_ERR_TRUNCATED          = 6,
    LS_NFC_ERR_TOO_LARGE          = 7,
    LS_NFC_ERR_INTERNAL           = 8,
} ls_nfc_error_t;

typedef struct {
    ls_nfc_tag_t                tag;
    bool                        has_ndef;
    bool                        writable;         /* T2T CC lock byte */
    size_t                      capacity_bytes;   /* declared, from CC */
    /* Raw NDEF message bytes, i.e. the value of the NDEF-message TLV.
       Valid until the next event fires. */
    const uint8_t              *ndef_bytes;
    size_t                      ndef_len;
    /* Records parsed out of ndef_bytes.  Views into ndef_bytes. */
    const ls_nfc_ndef_record_t *records;
    size_t                      record_count;
} ls_nfc_read_t;

/* --- Events ------------------------------------------------------------- */

typedef enum {
    LS_NFC_EV_DISCOVERED = 1,   /* tag entered the field, UID resolved */
    LS_NFC_EV_READ       = 2,   /* Type 2 CC + NDEF message succeeded */
    LS_NFC_EV_REMOVED    = 3,
    LS_NFC_EV_ERROR      = 4,
} ls_nfc_event_kind_t;

typedef struct {
    ls_nfc_event_kind_t kind;
    ls_nfc_tag_t        tag;    /* zeroed for REMOVED with no prior tag */
    ls_nfc_read_t       read;   /* filled for READ */
    ls_nfc_error_t      error;  /* filled for ERROR */
} ls_nfc_event_t;

typedef void (*ls_nfc_event_cb_t)(const ls_nfc_event_t *ev, void *user);

/* --- Config and lifecycle ---------------------------------------------- */

/* The board layer supplies the SPI device and IRQ pin.  The transport does
   not open the SPI bus itself - the board owns the bus and hands us a
   device handle.  spi_device_handle_t is left opaque here so this header
   stays free of driver includes.

   max_ndef_bytes bounds the message buffer allocated in PSRAM.  0 selects
   the default (LS_NFC_DEFAULT_MAX_NDEF).

   max_records bounds the records[] array parsed out of the message.
   0 selects the default (LS_NFC_DEFAULT_MAX_RECORDS). */

#define LS_NFC_DEFAULT_MAX_NDEF     2048
#define LS_NFC_DEFAULT_MAX_RECORDS  16
#define LS_NFC_MAX_NDEF_LIMIT       (16u * 1024u)

typedef struct {
    void   *spi_device;   /* spi_device_handle_t on device, may be NULL on host */
    int     irq_gpio;
    size_t  max_ndef_bytes;
    size_t  max_records;
    /* Polling cadence in ms while looking for a tag.  0 -> default. */
    uint32_t poll_ms;
} ls_nfc_config_t;

/* start/stop are the whole lifecycle.  start allocates the message buffer
   in PSRAM, the DMA-side buffers in internal RAM, installs the ISR and
   spawns the reader task; on any partial failure it releases every
   allocation and the RF field.  stop is safe to call from any thread and
   from the callback itself. */
int  ls_nfc_start(const ls_nfc_config_t *cfg,
                  ls_nfc_event_cb_t cb, void *user);
void ls_nfc_stop(void);
bool ls_nfc_running(void);

/* --- Pure NDEF parser (no radio, no SDK) -------------------------------
   Exposed on the public surface so a caller can re-parse a saved NDEF
   dump.  The parser writes at most out_cap records; if the message would
   yield more, LS_NFC_ERR_TOO_LARGE is returned. */

int ls_ndef_parse(const uint8_t *buf, size_t len,
                  ls_nfc_ndef_record_t *out, size_t out_cap,
                  size_t *out_count);

/* Convenience decoders.  Return true and fill outputs on success. */
bool ls_ndef_decode_text(const ls_nfc_ndef_record_t *r,
                         const uint8_t **lang, uint8_t *lang_len,
                         const uint8_t **text, size_t *text_len,
                         bool *utf16);

bool ls_ndef_decode_uri(const ls_nfc_ndef_record_t *r,
                        uint8_t *prefix,
                        const uint8_t **rest, size_t *rest_len);

/* --- Type 2 tag CC/TLV helpers (pure logic) ---------------------------- */

typedef struct {
    bool    valid;
    uint8_t magic;               /* 0xE1 when NDEF-formatted */
    uint8_t version_major;
    uint8_t version_minor;
    size_t  memory_size_bytes;   /* CC[2] << 3 */
    bool    read_only;           /* CC[3] & 0x0F != 0x00 */
} ls_t2t_cc_t;

/* mem is the flat Type 2 memory starting at page 0.  len is how many bytes
   are actually valid (page 0 through page N).  cc must have at least
   pages 0..3 available (16 bytes).  Returns 0 on success, non-zero
   ls_nfc_error_t on failure. */
int ls_t2t_parse_cc(const uint8_t *mem, size_t len, ls_t2t_cc_t *cc);

/* Walk TLVs starting at page 4 (byte 16) of mem.  On success sets
   *ndef_bytes / *ndef_len to the value of the NDEF-message TLV (T=0x03).
   Returns LS_NFC_ERR_TRUNCATED if the TLV runs past len, MALFORMED if the
   stream is invalid, TOO_LARGE if the NDEF message exceeds
   max_message_bytes, NONE if no NDEF TLV is present. */
int ls_t2t_find_ndef(const uint8_t *mem, size_t len,
                     size_t max_message_bytes,
                     const uint8_t **ndef_bytes, size_t *ndef_len);

#ifdef __cplusplus
}
#endif
#endif
