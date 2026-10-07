/* LR433: rtl_433's decoders for everyday devices - tyre pressure sensors,
   weather sensors, utility meters - fed by what the LR2021's packet engines
   hand over instead of by an SDR's samples.

   The chip is set up as a sampler. An OOK session runs at a bit rate well
   above the device's own (several decisions per pulse) and an FSK session at
   four times the device's chip rate, so what comes out of the FIFO is the
   on/off or mark/space waveform at a fixed rate; it is turned back into the
   pulse and gap widths rtl_433's slicers take, and each device's own decoder
   runs on the bits they make. Itron ERT is the exception: the OOK session
   runs at the meter's own chip rate and the Manchester pairs are read
   directly.

   Pure: no radio, no allocation, no globals written. The caller owns the
   work space, which is a few kilobytes and belongs off the stack. */

#ifndef LR433_DEC_H
#define LR433_DEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LR433_P_SCHRADER,          /* Schrader GG4, 315/433 OOK                 */
    LR433_P_SCHRADER_EG53MA4,  /* Schrader EG53MA4: GM 2006 on, OOK         */
    LR433_P_SCHRADER_SMD3MA4,  /* Schrader SMD3MA4: Subaru, Nissan, OOK     */
    LR433_P_GM_AFTERMARKET,    /* GM aftermarket, OOK                       */
    LR433_P_FORD,              /* Ford (Continental), FSK 19.2k             */
    LR433_P_TOYOTA,            /* Pacific PMV-C210, FSK 19.2k               */
    LR433_P_PMV107J,           /* Pacific PMV-107J: Toyota/Lexus US, FSK 10k */
    LR433_P_CITROEN,           /* Citroen/Peugeot VDO, FSK 19.2k            */
    LR433_P_HYUNDAI_VDO,       /* Hyundai VDO, FSK 19.2k                    */
    LR433_P_KIA,               /* Kia Rio and some Hyundai, FSK 20k         */
    LR433_P_ELANTRA2012,       /* Hyundai Elantra 2012 (TRW), FSK 20k       */
    LR433_P_ACURITE_TOWER,     /* Acurite 592TXR and kin, OOK PWM           */
    LR433_P_ACURITE_5N1,       /* Acurite 5-in-1 station, OOK PWM           */
    LR433_P_LACROSSE_TX141,    /* LaCrosse TX141 family, OOK PWM            */
    LR433_P_AMBIENT_F007TH,    /* Ambient Weather F007TH, OOK Manchester    */
    LR433_P_FINEOFFSET_WH2,    /* Fine Offset WH2/WH5/Telldus, OOK PWM      */
    LR433_P_AMBIENT_WH31E,     /* Ambient WH31E/Ecowitt WH31B, 915 FSK      */
    LR433_P_FINEOFFSET_WH24,   /* Fine Offset WH24/WH65 arrays, 915 FSK     */
    LR433_P_ERT_SCM,           /* Itron ERT SCM                             */
    LR433_P_ERT_SCMPLUS,       /* Itron ERT SCM+                            */
    LR433_P_ERT_IDM,           /* Itron ERT IDM                             */
    LR433_P_COUNT
} lr433_proto_t;

typedef enum { LR433_TPMS, LR433_SENSOR, LR433_METER } lr433_class_t;

/* What one session handed over. */
typedef enum {
    LR433_IN_OOK,    /* 1 = carrier on, `rate` decisions a second           */
    LR433_IN_FSK,    /* 1 = one tone, 0 the other: polarity is not trusted  */
    LR433_IN_ERT,    /* OOK at the ERT chip rate, after its detector        */
} lr433_in_t;

typedef struct {
    lr433_in_t kind;
    uint32_t rate;        /* decisions a second                             */
    /* What the chip matched and consumed before the payload, in time order
       (the first of `lead_bits` is bit lead_bits-1 of `lead`): the detector
       pattern or the sync word. It is part of the waveform. */
    uint32_t lead;
    uint8_t  lead_bits;
    const uint8_t *data;  /* the payload, MSB first in time                 */
    int bits;
} lr433_capture_t;

/* One decoded message. */
typedef struct {
    uint8_t proto;            /* lr433_proto_t                              */
    int8_t  channel;          /* -1 when the device has none                */
    int8_t  battery_ok;       /* 1 ok, 0 low, -1 not sent                   */
    char model[28];           /* rtl_433's model name                       */
    char id[16];              /* the ID as rtl_433 prints it                */
    float kpa;                /* tyre pressure; NAN when not sent           */
    float temp_c;             /* NAN when not sent                          */
    float humidity;           /* %RH; NAN when not sent                     */
    float rain_mm, wind_ms;    /* accumulated rain and average wind; NAN     */
    int64_t consumption;      /* meter reading in the meter's units; -1     */
} lr433_msg_t;

/* The work space: pulse widths and bit rows for one capture. */
#define LR433_MAX_PULSES 1200
#define LR433_BB_ROWS    16
#define LR433_BB_COLS    128   /* bytes: 1024 bits a row */

typedef struct {
    uint16_t num_rows, free_row;
    uint16_t bits_per_row[LR433_BB_ROWS];
    uint16_t syncs_before_row[LR433_BB_ROWS];
    uint8_t  bb[LR433_BB_ROWS][LR433_BB_COLS];
} lr433_bitbuf_t;

typedef struct {
    uint32_t rate;
    int n;
    uint16_t pulse[LR433_MAX_PULSES];
    uint16_t gap[LR433_MAX_PULSES];
} lr433_pulses_t;

typedef struct {
    lr433_pulses_t p;
    lr433_bitbuf_t bits;
} lr433_work_t;

/* Decode one capture. Writes up to `max` distinct messages; returns how
   many. A message heard twice in one capture (devices repeat) is one. */
int lr433_decode(const lr433_capture_t *cap, lr433_work_t *w, lr433_msg_t *out, int max);

/* rtl_433's name for the protocol family, and a label of at most 9
   characters for a table column. */
const char *lr433_proto_name(int proto);
const char *lr433_proto_label(int proto);
lr433_class_t lr433_proto_class(int proto);

/* Exposed for the bench. */
uint8_t  lr433_crc8(const uint8_t *m, unsigned n, uint8_t poly, uint8_t init);
uint16_t lr433_crc16(const uint8_t *m, unsigned n, uint16_t poly, uint16_t init);
/* Width and gap runs of 1s and 0s, from the lead bits then the data, from the
   first 1 on. `invert` swaps the two levels. Returns the pulse count. */
int lr433_pulses(const lr433_capture_t *cap, bool invert, lr433_pulses_t *p);

#ifdef __cplusplus
}
#endif

#endif /* LR433_DEC_H */
