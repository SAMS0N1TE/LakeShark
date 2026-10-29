/* TIME MACHINE replay into the pager decoders; see ls_tmach_decode.c. */
#ifndef LS_TMACH_DECODE_H
#define LS_TMACH_DECODE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Hands TIME MACHINE the POCSAG replay chains and what names the decoder a
   radio is running, for a clip's "decoder". Once, at boot. */
void ls_tmach_decode_register(void);

#ifdef __cplusplus
}
#endif

#endif
