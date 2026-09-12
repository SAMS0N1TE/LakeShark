# Independent DMR wire fixtures

These are generated outputs of **unmodified upstream encoders**, not LakeShark
encode/decode round trips and not over-the-air captures. The ordinary host test
uses the checked-in `dmr_reference_vectors.h`; it requires no network or tools
from `work/`.

Primary reference: [MMDVM-Host commit 590c531391dfd3146073afbc3956f70d42c62a46](https://github.com/g4klx/MMDVM-Host/tree/590c531391dfd3146073afbc3956f70d42c62a46).

- [Golay2087.cpp](https://github.com/g4klx/MMDVM-Host/blob/590c531391dfd3146073afbc3956f70d42c62a46/Golay2087.cpp): all 256 Slot Type codewords.
- [BPTC19696.cpp](https://github.com/g4klx/MMDVM-Host/blob/590c531391dfd3146073afbc3956f70d42c62a46/BPTC19696.cpp) and [Hamming.cpp](https://github.com/g4klx/MMDVM-Host/blob/590c531391dfd3146073afbc3956f70d42c62a46/Hamming.cpp): six 33-byte burst payloads.
- [RS129.cpp](https://github.com/g4klx/MMDVM-Host/blob/590c531391dfd3146073afbc3956f70d42c62a46/RS129.cpp), [DMRFullLC.cpp](https://github.com/g4klx/MMDVM-Host/blob/590c531391dfd3146073afbc3956f70d42c62a46/DMRFullLC.cpp), and [DMRDefines.h](https://github.com/g4klx/MMDVM-Host/blob/590c531391dfd3146073afbc3956f70d42c62a46/DMRDefines.h): RS byte ordering and header/terminator masks.

The referenced implementation files are GPL-2.0-or-later, copyright Jonathan
Naylor G4KLX (BPTC also credits Ian Wraith). They are downloaded only for optional
regeneration, not vendored into the firmware. The small generator supplies only
byte/bit conversion helpers; the FEC encoders themselves are upstream code.

The six LC vectors use three explicitly chosen nine-byte messages, each with
the voice-header mask 0x96 and terminator mask 0x99. Burst sync/Slot Type regions
are zero in these BPTC vectors: they test the split payload layout, not sync
acquisition. The third message exercises arbitrary payload bits; it is not a
claim that every FLCO/FID value is supported for semantic call handling.

## Reproduce

From the firmware tree, using PowerShell, Python and g++:

```powershell
$ref = '590c531391dfd3146073afbc3956f70d42c62a46'
New-Item -ItemType Directory -Force work/dmr-reference | Out-Null
foreach ($f in @('Defines.h','Utils.h','Golay2087.cpp','Golay2087.h','BPTC19696.cpp','BPTC19696.h','Hamming.cpp','Hamming.h','RS129.cpp','RS129.h')) {
    Invoke-WebRequest "https://raw.githubusercontent.com/g4klx/MMDVM-Host/$ref/$f" -OutFile "work/dmr-reference/$f"
}
g++ -std=c++11 -I work/dmr-reference bench/fixtures/dmr_reference_generate.cpp work/dmr-reference/BPTC19696.cpp work/dmr-reference/Hamming.cpp work/dmr-reference/Golay2087.cpp work/dmr-reference/RS129.cpp -o work/dmr-reference/generate.exe
& work/dmr-reference/generate.exe > work/dmr-reference/vectors.txt
python -I bench/fixtures/dmr_reference_pack.py work/dmr-reference/vectors.txt work/dmr-reference/regenerated.h
```

Compare `regenerated.h` byte-for-byte with the checked-in header. The packer also
checks all 256 upstream Slot Type words against the degree-11 extended form.

Reference SHA-256 hashes (raw downloaded bytes):

| File | SHA-256 |
|---|---|
| Golay2087.cpp | 1cae4c93e00644664bfccd4f695269f67d347815b9a88fa3cb8edb147a32e961 |
| BPTC19696.cpp | ebc9c88efda5c13148a91a8b9ae40de006356ec32bfb1979bf943474c93bee96 |
| Hamming.cpp | fa75019ef72f8c6cebdd2f5115e974ef8b99a59119db329d3b1cfdfe50425097 |
| RS129.cpp | 3cb7f8315e9583a3ab28fab2d5947808e3a282020e2c92ef1df7a8371aea11cf |
| DMRFullLC.cpp | 283cf122952a83e0259a3d48b70772eba0a5ca57ff0be89493df12976f0477bc |
| DMRDefines.h | a252dcde4c8fc927ef1bec347c30b0f91291f3d8a72f7db836c3a08cb9c96a39 |

## Findings and limits

Before the fixes, two independent cases failed with 970 assertions: 254/256
Slot Type encoder outputs differed, and the BPTC column code contained an extra
data-bit term. Existing 15 self-generated cases passed throughout. After the
fixes and checked LC API, all 18 targeted cases pass. Coverage includes all
single-bit positions of all six BPTC fixtures, all single-bit LC/parity
corruptions, wrong burst masks, and preserving output on rejection.

This anchors interoperability to the named upstream implementation, not an
independent formal ETSI conformance certification. No RF acquisition, timing,
slot identification, embedded LC, RS correction, or AMBE voice is implemented
or verified here. The raw `dmr_lc_parse()` remains available for old fixtures;
receiver code must use `dmr_lc_decode()` and separately validate burst type and
supported FLCO/FID semantics before publishing talkgroup/source identity.
