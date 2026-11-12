# Argentina scanner/SDR frequency list: sources and confidence

Compiled on 2026-11-10. Files:

- `freqs_argentina.csv` holds the frequencies.
- `freqs_argentina_bands.csv` holds band edges: cellular, ISM, amateur, airband, marine, broadcast, weather and satellite.

Rules I followed:

- Every row traces to a source listed below.
- If a row rests on only one non-official source, or the Argentine use of a channel is not confirmed, its `notes` field says "unverified" (or "may be outdated" or "not verified").
- Nothing was invented. Anything searched for and not found is listed under **Not found** for each group.

Tags: `listen` marks receive-only rows, and `tx-ok` marks the license-free allocations noted below. `hf` marks rows below 24 MHz, which need the HackRF because the R820T starts at 24 MHz.

**Rows per group (566 in total; the band file has 37 ranges):**

| Group | Rows | Group | Rows |
|---|---|---|---|
| AVIATION | 174 | BROADCAST FM | 111 |
| AMATEUR | 80 | BROADCAST AM | 53 |
| PMR/LICENSE-FREE | 55 | MARINE | 38 |
| LORA/MESH | 22 | NAVIGATION | 11 |
| SATELLITE | 11 | WEATHER | 5 |
| EMERGENCY | 4 | ISM/IOT | 2 |
| PUBLIC SAFETY / RAIL / UTILITY / CELLULAR | 0 (see below) | | |

---

## AVIATION (174 rows): confidence HIGH for aerodrome and ACC values

**Primary source: ANAC AIP Argentina (eAIP).** The index is at https://ais.anac.gob.ar/aip, and the AD list is served from https://ais.anac.gob.ar/aip/ad. I downloaded the AD 2.0 "Datos del AD" PDFs on 2026-11-10 and used section AD 2.18 (ATS communication facilities) for frequencies and AD 2.2 for the ARP coordinates used as lat/lon. The AD 2.18 pages I read carry AMDT AIRAC 1/2026 (11 Jun 2026) or earlier stamps.

| ICAO | AD 2.0 PDF |
|---|---|
| SABE Aeroparque | https://ais.anac.gob.ar/descarga/aip-6a10684a7a5bd |
| SAEZ Ezeiza | https://ais.anac.gob.ar/descarga/aip-69e10875b7c89 |
| SADF San Fernando | https://ais.anac.gob.ar/descarga/aip-6a68fbe672145 |
| SADP El Palomar | https://ais.anac.gob.ar/descarga/aip-69e26822c76b6 |
| SADM Morón | https://ais.anac.gob.ar/descarga/aip-69e135e507da0 |
| SADJ Mariano Moreno | https://ais.anac.gob.ar/descarga/aip-69e16419c624e |
| SACO Córdoba | https://ais.anac.gob.ar/descarga/aip-69e1078aa7f9d |
| SAAR Rosario | https://ais.anac.gob.ar/descarga/aip-69e105dba2f99 |
| SAME Mendoza | https://ais.anac.gob.ar/descarga/aip-69fc9d7664864 |
| SAZS Bariloche | https://ais.anac.gob.ar/descarga/aip-69e8bacf19c28 |
| SAZM Mar del Plata | https://ais.anac.gob.ar/descarga/aip-69e1232468b6a |
| SASA Salta | https://ais.anac.gob.ar/descarga/aip-69e1138586777 |
| SANT Tucumán | https://ais.anac.gob.ar/descarga/aip-69e10b1142729 |
| SAWH Ushuaia | https://ais.anac.gob.ar/descarga/aip-69e7b2b7ee74b |
| SAZN Neuquén | https://ais.anac.gob.ar/descarga/aip-69e1234aab580 |
| SARI Iguazú | https://ais.anac.gob.ar/descarga/aip-69e6163c31b23 |
| SAVC Comodoro Rivadavia | https://ais.anac.gob.ar/descarga/aip-69e1143c25a85 |
| SAWC El Calafate | https://ais.anac.gob.ar/descarga/aip-69e7b256c1c2c |
| SAZB Bahía Blanca | https://ais.anac.gob.ar/descarga/aip-69e7b314ac7f0 |
| SAWG Río Gallegos | https://ais.anac.gob.ar/descarga/aip-69e1165d02be1 |
| SANU San Juan | https://ais.anac.gob.ar/descarga/aip-69e10b2277811 |
| SAVT Trelew | https://ais.anac.gob.ar/descarga/aip-69e11508bc15d |
| SARP Posadas | https://ais.anac.gob.ar/descarga/aip-69e1137370dba |
| SASJ Jujuy | https://ais.anac.gob.ar/descarga/aip-69e113f9e6050 |

**Other AIP parts used:**

- **AIP ENR 2.1 (FIR, UIR, TMA, CTA)**, https://ais.anac.gob.ar/descarga/aip-69e11f6302403, pages stamped AMDT AIRAC 2/2025. It supplied:
  - ACC sector frequencies: Ezeiza Centro North and South, CTA Ezeiza I–IV, Baires Control (TMA Baires) North and South, Córdoba, Mendoza, Resistencia, and Comodoro Rivadavia North and South.
  - TWR/TMA frequencies for 19 smaller aerodromes whose AD 2.18 I did not download: SAWE, SAVE, SAVV, SAAP, SAZR, SAZT, SAZY, SAAC, SANC, SANL, SAOC, SANE, SAOU, SAMR, SAMM, SATR, SARL, SAAG and SAWB, plus TMA Resistencia. The coordinates for these are approximate airport coordinates, not AIP values.
- **AIP GEN 3.4**, https://ais.anac.gob.ar/descarga/aip-68de72f2a4593 (AMDT AIRAC 1/2025):
  - 121.5 emergency.
  - 123.10 MHz (with 3023 and 5680 kHz) for SAR on-scene communications.
  - **123.45 MHz** as the SAM-region in-flight broadcast (TIBA) frequency during ATS contingency.
  - DCL departure clearances delivered over ACARS at SABE/SAEZ.
- **AIP supplements in force** (https://ais.anac.gob.ar/sup, list "a"), checked against the AD 2.18 data:
  - SUP A125/26 changes the **Salta ATIS to 127.75 MHz** (the CSV uses it): https://ais.anac.gob.ar/descarga/sup-6a174d25e3bdc
  - SUP A199/26 confirms the Tucumán ATIS on 127.55.
  - SUP A212/26: **El Calafate ATIS 127.85 U/S until Aug 2027**.
  - SUP A122/26: TMA Esquel provides no ATS because of a lack of comms coverage.
  - SUP A106/26, A107/26, A108/26 and A111/26: ATS contingency is active in parts of FIR Ezeiza (the area between the Tandil and Mar del Plata TMAs), FIR Mendoza and FIR Córdoba. Aircraft there self-announce on 123.45.
  - SUP A172/26 (TMA Posadas) and A127/26 (TMA Paraná) keep the same frequencies.
- **Navaids (NAVIGATION group):** the VOR/ILS frequencies come from the same AD 2.19 sections.

**Notes and caveats:**

- **8.33 kHz:** no 8.33 kHz assignments appear in the AIP. Every published channel sits on the 25 kHz raster (for example 127.625 at El Palomar), so `bw_khz=25`.
- **ATIS:** SABE lists two ATIS frequencies (127.60 and 127.90) and I included both. No ATIS is published for SAZM, SAVC or SANU.
- **Ground (SMC):** none is published for SASA, SAVC, SAWH or SAWC. SAZB's 121.90 is marked U/S. For SAZS the AIP gives SMC as 119.10, the same as the TWR frequency.
- **ACC rows** carry approximate sector centres and radii, for location filtering only.
- **ACARS 131.550 / VDL2 136.975:** the AIP only confirms that ACARS is used (DCL). 131.550 as the South American primary ACARS channel and 136.975 as the worldwide VDL2 common signalling channel come from https://www.sigidwiki.com/wiki/ACARS, https://sigidwiki.com/wiki/VHF_Data_Link_%28VDL2%29 and https://github.com/szpajder/dumpvdl2. Other ACARS or VDL2 channels used by Argentine ground stations are **unverified** and left out.
- **ADS-B:** 1090ES only. **978 MHz UAT is a US-only system and is not used in Argentina**, so it is omitted and noted in the 1090 row.
- **Change risk:** the AIRAC cycle changes data every 28 days, and frequencies can change by NOTAM. EANA's NOTAM summaries for June 2026 mention ATM system work at Aeroparque TWR (https://www.eana.com.ar/sites/default/files/2026-06/PDA%2025-06-2026.pdf).

**Not found or not included:** delivery frequencies for airports other than SABE, SAEZ, SADF and SACO (none are published). Military-only UHF ATC frequencies. ELT 121.5 homing is covered by the 121.5 row.

## NAVIGATION (11 rows): HIGH

- **VOR/ILS:** EZE, CBA, ROS, DOZ, MDP, BAR, USU and the AEP ILS 13, all from AIP AD 2.19 (PDFs above). Note that SUP A252/26 marks VOR UIS (San Luis) U/S, and it is not in the CSV.
- **GNSS:** GPS L1 1575.42, GLONASS L1 and BeiDou B1I are standard published signal centres (ICDs). They are not Argentina-specific.

## MARINE (38 rows): HIGH for VTS channels, MEDIUM for recreational channels

**Prefectura Naval Argentina VTS pages**, read 2026-11-10. Each gives the VHF channel, call sign, coverage and the CH15 broadcast schedule:

- Index: https://www.argentina.gob.ar/node/139214 (Centros VTS)
- Río de la Plata (L2G: CH12, CH09, CH72, CH81 157.075/161.675): https://www.argentina.gob.ar/node/174386
- Zárate (L5T: CH72, CH14): https://www.argentina.gob.ar/node/174387
- San Nicolás (L6G CH12; Villa Constitución L6H CH09; Ramallo L6F CH09): https://www.argentina.gob.ar/node/174388
- Rosario (L6I CH14): https://www.argentina.gob.ar/node/174391
- Corrientes (L6Y CH12; CH14 safety info and river heights): https://www.argentina.gob.ar/node/174394
- San Pedro (L6E CH12): https://www.argentina.gob.ar/node/174398
- Diamante (L6M CH12): https://www.argentina.gob.ar/node/174405
- Mar del Plata (L2U CH09; L2T 4354/8713 kHz; CH15 broadcasts): https://www.argentina.gob.ar/prefecturanaval/avisos/mar_del_plata
- Bahía Blanca (L2N CH12): https://www.argentina.gob.ar/node/174381
- Comodoro Rivadavia (L3B CH09): https://www.argentina.gob.ar/node/174379
- Ushuaia (L3P CH12; Beagle coast stations L4A–L4D; CH15 broadcasts): https://www.argentina.gob.ar/node/174378
- PNA safety pages (CH16 and phone 106): https://www.argentina.gob.ar/node/235064 and https://www.argentina.gob.ar/node/295770
- PNA national AIS server: https://www.argentina.gob.ar/prefecturanaval/ais

**RESMMA (Reglamento del Servicio Móvil Marítimo)**, SHN publication H.236, Decreto 2174/84. Copy used: https://campus.mdp.utn.edu.ar/pluginfile.php/180811/mod_book/chapter/817/RESMMA%20-%20Reglamento%20de%20Servicio%20Movil%20Maritimo.pdf. Its SECONADE recreational service assigns:

- CH74 to class A clubs, and also as the PNA working channel.
- CH71 to class B clubs.
- CH78B to marinas, with CH78A as reserve.
- CH06 for boat-to-boat.

These rows are marked "RESMMA 1984; may be outdated".

**ITU RR Appendix 18 channels** CH13, 67, 68, 69, 73 and 77 are included because they were requested, but I found **no Argentine-specific assignment** for them, and their rows say so.

**Not found:** Prefectura's own coast-station working-channel table, harbour or pilot channels for the Buenos Aires port beyond the VTS pages, and port-operator (AGP) or tug channels. RESMMA (1984) mentions a La Plata coast station (L5F) on CH09; I left it out because I could not confirm it is current. HF voice is limited to 2182/4125 kHz and the Mar del Plata VTS, all HackRF-only. NAVTEX 518/490 kHz is out of range for this hardware.

## AMATEUR (80 rows) and amateur SATELLITE (10 rows): MEDIUM-HIGH

- **ENACOM Reglamento de Radioaficionados, Anexo A "Plan de Bandas"** (IF-2018-26956961): https://www.qsl.net/bandasaltas/files/Plan%20de%20Bandas%20Res%20508a3.pdf. This is the source for band edges, repeater sub-bands and offsets (2 m ±600 kHz, 70 cm +5 MHz, 1.25 m −1.6 MHz, 6 m −1 MHz), calling frequencies (2 m FM **146.520**, SSB 144.200, 6 m 50.110/51.500, 1.25 m 223.500, 70 cm 432.100) and the IARU emergency centres of activity.
- Res ENACOM 1186/2024 (https://www.boletinoficial.gob.ar/detalleAviso/primera/317598/1) and Res 481/2026 (https://www.argentina.gob.ar/normativa/nacional/norma-427439/texto) do not change the band plan.
- **Repeaters:** RepeaterBook Argentina (295 entries, fetched 2026-11-10): https://www.repeaterbook.com/row_repeaters/Display_SS.php?lang=es&state_id=AR. Detail pages follow the pattern `https://www.repeaterbook.com/row_repeaters/details.php?state_id=AR&ID=<RB#>`, and each row cites its RB number and update date. Simplex nodes: https://www.repeaterbook.com/simplex_nodes/index.php?state_id=AR.
- Radio Club Argentino's own page confirms LU4AA (146.910, −600, 123.0 Hz, FM/C4FM): https://www.lu4aa.org/wp/rpt-vhf/
- **Cross-check lists (old):**
  - The CNC licensed-repeater list for 2004–2008: https://www.qsl.net/lu3jkv/rpt.htm
  - LU7DOT, 2010: https://www.qsl.net/lu7dot/repetidoras.htm
  - The older CNC Res 50/98 band plan: https://www.qsl.net/lu7dot/bandas.htm
  - The 6 m page: https://www.qsl.net/lu_6_meters/
- **APRS conflict:** the ENACOM plan text says **144.390**, but AMSAT-UK (2020, https://amsat-uk.org/2020/01/29/amsat-argentina-drifting-buoy-on-145-825-mhz-aprs/) and https://www.sigidwiki.com/wiki/APRS call **144.930** Argentina's APRS frequency. Both are listed with the conflict noted. aprs.fi blocked automated access, so I could not see on-air evidence.
- **Satellites:**
  - ARISS contact page: https://www.ariss.org/contact-the-iss.html
  - ARISS status page (2026-11-04): https://www.ariss.org/current-status-of-iss-stations.html. The ISS cross-band repeater uplink tone is **67 Hz, not 88.5**. ISS APRS is currently on **437.825** (still under test).
  - AMSAT live FM, linear and digipeater lists (updated 2026-10-31): https://www.amsat.org/live-fm-satellites/, https://www.amsat.org/live-linear-satellites/ and https://www.amsat.org/live-digipeater-satellites/
  - AMSAT status grid: https://status.amsat.org/. It shows SO-50, AO-123, RS-44, FO-29, AO-7 and JO-97 heard in Nov 2026.
- **Bandwidth:** RepeaterBook marks Argentine analog repeaters as 25 kHz FM, so terrestrial analog NFM and APRS rows use `bw_khz=25`. DMR, C4FM and D-STAR rows use 12.5.

**Not found or unverified:**

- Current club web pages for the Córdoba, Rosario, Mendoza, Mar del Plata, Neuquén and Bahía Blanca clubs. Those rows rest on RepeaterBook plus the old CNC list.
- Any Ushuaia repeater. Only the LU8XW EchoLink simplex node was found.
- Only one current Tucumán repeater.
- Any 6 m repeater on air. LU5CBA 53.070 comes from old lists only and is marked unverified.
- HF net schedules for 40 m and 80 m.
- A BrandMeister repeater list (DMR details come from RepeaterBook).
- **Tone conflicts** between RepeaterBook and the CNC list are flagged in the notes: LU4HH, LU4OC, LU3DY, LU7DBL and LU1DBQ.
- Satellites left out as dying, unreliable or out of reach: ISS SSTV, AO-91, PO-101, TEVEL2 and IO-86.

## PMR / LICENSE-FREE (55 rows)

- **FRS-type "uso familiar", channels 1–7 (462.5625–462.7125): HIGH.** Resolución SC 2750/98 authorizes 462.55625–462.71875 MHz on a secondary basis, with **no individual authorization** if the radio is homologated, 500 mW, a fixed antenna of at most λ/4, simplex, 11 kHz bandwidth and at most 3 minutes per transmission. Text: https://www.argentina.gob.ar/normativa/nacional/norma-55385/texto. It was kept in force by Res ENACOM 2040/2019 (https://www.boletinoficial.gob.ar/detalleAviso/primera/207308/20190510), which also cites MERCOSUR GMC Res 06/02. These rows are tagged **tx-ok**. FRS channels 8–22 and the GMRS 467 MHz inputs are **not** authorized and are not listed.
- **PMR446, channels 1–8: LOW.** I found **no ENACOM rule authorizing PMR446**. The rows are listen-only because imported radios are on the air.
- **CB "Banda Ciudadana" (SBC), 40 channels AM: MEDIUM.** Res ENACOM 481/2026 sets fees for each station authorized in the Servicio de Banda Ciudadana, so **CB in Argentina needs authorization** and the rows are tagged `listen`. I could not find the Argentine SBC technical rules or channel table. The CSV uses the standard 40-channel 26.965–27.405 plan, marked "AR channel table unverified".

## ISM / IOT (2 rows) and LORA / MESH (22 rows)

**ISM / IOT:**

- **915–928 MHz** and **2400–2483.5 MHz** (also 5725–5850) are declared **shared use without authorization** by Res. 581/2018 of the ex Ministerio de Modernización (RESOL-2018-581-APN-MM), with a limit of 30 dBm conducted / 36 dBm EIRP. Annex: https://argentina.gob.ar/normativa/314174_res581_pdf/archivo. Summary: https://www.marval.com/Publicacion/nueva-resolucion-que-favorece-el-desarrollo-de-la-internet-de-las-cosas-13246. Confidence: HIGH.
- **902–915 MHz** is available only to low-power devices under a field-strength limit (ENACOM low-power device rules, referenced at https://www.boletinoficial.gob.ar/detalleAviso/primera/292414/20230818). A 2017 resolution earmarked 905–915 for IMT. MEDIUM.
- **433.075–434.775 MHz** is for low-power devices under ENACOM technical standard Q2-60.14 and Res 5186-E/2017 (the power update): https://ib-lenhardt.com/news/new-regulation-for-low-power-devices-2 and https://cetecomadvanced.com/en/news/regulatory-changes-for-low-power-devices-in-argentina/. MEDIUM, since the norm text itself was not read.

**LoRaWAN:** The Things Network's frequencies-by-country table maps **Argentina to AU915-928** and cites RESOL-2018-581-APN-MM: https://www.thethingsnetwork.org/docs/lorawan/frequencies-by-country/. The channels come from the TTN AU915 plan, sub-band 2: https://www.thethingsnetwork.org/docs/lorawan/frequency-plans/. HIGH.

**Meshtastic:**

- The region table is at https://meshtastic.org/docs/configuration/radio/lora. There is no official Argentina entry; ANZ covers 915–928.
- Preset parameters come from https://meshtastic.org/docs/overview/radio-settings: LongFast is SF11 / 250 kHz / CR 4/5, and MediumFast is SF9 / 250 kHz / CR 4/5.
- An Argentine community uses **ANZ + LongFast**: the Bahía Blanca TTN community post of 2025-02-18, https://www.thethingsnetwork.org/community/bahia-blanca/post/meshtastic-bahia-blanca.
- The default frequencies are **computed** from the firmware's djb2 hash of the preset name and the region band. The method was validated against the docs' US LongTurbo value (slot 14, 908.750). ANZ LongFast is **919.875 MHz** (slot 20) and ANZ MediumFast is 926.125 (slot 45).
- Confidence: MEDIUM-HIGH for ANZ LongFast and LOW for AR use of MediumFast. US LongFast 906.875 is listed as listen-only, because 902–915 is not in the shared band.

**MeshCore:** I found **no Argentina preset**. The rows give the USA/Canada preset (910.525, SF7 BW62.5) and the Australia preset (915.8 MHz), both from community wikis and commits (https://wiki.eastmesh.au/meshcore/radio-layer/, https://areyoumeshingwith.us/docs/meshcore/regional-settings/), and are marked unverified for AR.

**Not included:** 868 MHz (EU) devices, which are not allowed in AR, and Meshtastic 2.4 GHz LORA_24, whose default slot I could not verify.

## SATELLITE (11 rows) and WEATHER (5 rows)

**NOAA APT:**

- NOAA-18 was decommissioned on 6 Jun 2025, NOAA-19 on 13 Aug 2025 and NOAA-15 on 19 Aug 2025: https://www.nesdis.noaa.gov/news/suspension-of-poes-data-users-after-june-16-2025-and-decommissioning, https://www.ospo.noaa.gov/Operations/POES/status.html and https://www.arnewsline.org/news-text/2025/8/14/us-legacy-weather-satellites-being-decommissioned.
- NOAA-15 and NOAA-18 rows are kept, marked "(off)", so old lists can be reconciled.
- 137.100 is relabelled as Meteor LRPT. NOAA-19 is off.

**Meteor-M N2-3 and N2-4 LRPT on 137.900 / 137.100 MHz:** sources are WMO OSCAR (https://space.oscar.wmo.int/satellites/view/meteor_m_n2_3 and https://space.oscar.wmo.int/satellites/view/meteor_m_n2_4) and SatNOGS (https://db.satnogs.org/satellite/57166/ and https://db.satnogs.org/satellite/59051). Which frequency each satellite uses changes, so check recent reports. MEDIUM.

**GOES-19:**

- GOES-19 has been operational GOES-East since 7 Apr 2025: https://www.ospo.noaa.gov/data/messages/2025/04/MSG_20250407_1510.html
- HRIT/EMWIN is on 1694.1 MHz and GRB on 1686.6: https://space.oscar.wmo.int/satellites/view/goes_19
- The list's mode set has no HRIT entry, so the row is labelled `LRPT` and the notes say it is actually HRIT BPSK and needs a dish.

**Iridium:** the main downlink channels sit in 1616–1626 MHz, and 12 simplex channels occupy 1626.0–1626.5. Channel 7, at 1626.270833 MHz, is the ring-alert channel. Sources: https://sigidwiki.com/wiki/Iridium and https://github.com/muccc/gr-iridium. MEDIUM-HIGH.

**Inmarsat:** only a band-edge entry (1525–1559). AR-visible channel frequencies are not verified.

**Radiosondes:** the band row is 400.15–406. SMN sounding sites come from the UCAR/EOL SMN radiosonde dataset (https://data.eol.ucar.edu/dataset/553.007). Per-flight frequencies are not published.

## EMERGENCY (4 rows)

- 121.5 is in AVIATION and CH16 and CH70 are in MARINE (above).
- 243.0 MHz is the ITU military aero emergency frequency.
- Cospas-Sarsat 406.0–406.1 is digital, actually BPSK, but the row is labelled `FSK` because the mode list has no PSK.
- 3023 and 5680 kHz SAR come from AIP GEN 3.4 and need the HackRF.

## CELLULAR (band edges only, in `freqs_argentina_bands.csv`)

- **700 MHz band 28:** Movistar 758–768 down / 703–713 up, Personal 768–778 / 713–723, Claro 778–793 / 723–738 (2015 award). Sources: https://www.telesemana.com/blog/2015/06/12/argentina-adjudico-espectro-en-la-banda-de-700-mhz/ and https://www.telecompaper.com/news/argentina-finalises-award-of-4g-spectrum--1087304. The holder of 793–803 is **not verified**.
- **AWS:** Movistar holds 2110–2120 (2014). The rest of the split is not verified.
- **3.5 GHz:** Claro 3300–3400, Personal 3400–3500, Movistar 3550–3600 (https://dplnews.com/?p=211546). Who holds 3500–3550 is not verified.
- **B5, B2 and B7:** the edges are 3GPP band definitions. The per-operator split is not verified.

## PUBLIC SAFETY / RAIL / UTILITY: no frequency rows (by design)

I found no publicly published conventional frequencies for Argentine police, fire, rail or utilities, so none were listed.

**Documented digital trunked systems (frequencies not published):**

- Mendoza province runs a **TETRA** network shared by police, fire and Gendarmería, with 61 base sites and about 85% coverage: https://www.mendoza.gov.ar/prensa/seguridad-presento-la-ampliacion-de-la-red-tetra-en-mendoza/ and https://www.criticalcommunicationsreview.com/ccr/news/112372/government-praises-argentinian-mendoza-tetra-critical-communications-system
- Santa Fe uses Motorola **Dimetra TETRA**: https://www.fie.undef.edu.ar/ceptm/?p=10821
- San Luis uses **TETRA**, interoperating with Santa Fe, Córdoba and San Juan: https://www.motorolasolutions.com/newsroom/press-releases/san-luis-province-strengthens-public-safety.html
- Tucumán police are digitising for privacy: https://www.lagaceta.com.ar/nota/877758/actualidad/digitalizaran-red-comunicaciones-policia-para-mayor-privacidad-seguridad.html

**Not found:** systems used by Policía de la Ciudad (CABA), Policía Bonaerense, Policía Federal, Bomberos Voluntarios, SAME (CABA EMS), Trenes Argentinos, Subte or utilities. I found no ENACOM resolution with a national volunteer-firefighter channel.

## BROADCAST FM (111 rows) and BROADCAST AM (53 rows): MEDIUM-HIGH

**ENACOM has no public station database.** I found no FM station CSV on datos.gob.ar (checked through the CKAN API), and enacom.gob.ar has no station search; its "Datos Abiertos / Licencias audiovisuales" link points to a placeholder page.

The best official list is the **ENACOM-compiled station list for the Oct 2025 elections**, published by DINE as one PDF per province. Each entry gives the ENACOM code, AM/FM and a station name that includes the frequency. This source is coded **ENACOM25** in `notes`.

- Index: https://www.argentina.gob.ar/dine/listado-de-servicios-de-comunicacion-audiovisual
- CABA example: https://www.argentina.gob.ar/sites/default/files/2025/08/listado_definitivo_ciudad_autonoma_de_buenos_aires.pdf (other provinces follow the same file-name pattern)

Each row's `notes` ends with `src` codes for its sources:

| Code | Source | What it confirmed |
|---|---|---|
| ENACOM25 | DINE/ENACOM 2025 election-airtime lists, above | Official, compiled Aug–Sep 2025 |
| ENACOM16 | Res. ENACOM 10484-E/2016, https://www.boletinoficial.gob.ar/pdf/linkQR/c2M0TmRpTm9uQUUrdTVReEh2ZkU0dz09 | Cadena 3 network licences and call signs |
| HCDN24 | Chamber of Deputies bill 6040-D-2024, https://www4.hcdn.gob.ar/dependencias/dsecretaria/Periodo2024/PDF2024/TP2024/6040-D-2024.pdf | Radio Nacional local FMs switched in 2024 to relay Nacional Rock or Folklórica |
| ESWIKI | https://es.wikipedia.org/wiki/Radio_Nacional_(Argentina), https://es.wikipedia.org/wiki/Medios_de_comunicaci%C3%B3n_de_la_ciudad_de_Buenos_Aires, https://es.wikipedia.org/wiki/Radio_Perfil, https://es.wikipedia.org/wiki/Cadena_3_Argentina | Cross-check |
| WRM | worldradiomap.com Argentina city pages, https://worldradiomap.com/ar/ | Cross-check listing |
| SITE | The station's own site: metropowerhits.com, fmaspen.com, urbanaplayfm.com, radioconvos.com.ar, fmrockandpop.com, fmmilenium.com.ar, rivadavia.com.ar, continental.com.ar, lared.am, pagina12.com.ar/am750, lt8.com.ar, radionihuil.com.ar, buenosaires.gob.ar (La 2x4) | Frequency stated by the station |
| R10 | https://www.radio10.com.ar/repetidoras | Radio 10's official repeater list |
| TA26 | Tiempo Argentino, https://www.tiempoar.com.ar/ta_article/verano-en-las-fm/ (Feb 2026) and https://www.tiempoar.com.ar/ta_article/am-2026/amp/ (Jan 2026) | 2026 dial changes |
| LN26 | La Nación, 27 Jan 2026, https://www.lanacion.com.ar/espectaculos/nacional-am-870-define-su-programacion-con-algunas-bajas-y-nuevas-voces-como-la-de-adrian-korol-nid27012026/ | Nacional 870, AM 1070, 770, 990, 1030 |
| IPRO26 | iProfesional, Sep 2026, https://www.iprofesional.com/negocios/463541-ciudad-relanza-concesion-de-radios-tras-fracasar-primera-licitacion | City radios AM 1110 / FM 92.7 |
| ECON25 | El Economista, Nov 2025, https://eleconomista.com.ar/actualidad/cnn-va-radio-argentina-pasara-am-950-2026-n90520 | CNN Radio leaving AM 950 |
| LETRAP26 | Letra P, Jun 2026, https://www.letrap.com.ar/medios/lt3-rosario-reemplazo-programas-locales-contenidos-portenos-y-el-gremio-reclama-los-puestos-trabajo-n5424324 | LT3 680 relays Rivadavia |
| INFONEG | infonegocios.info Córdoba FM list (2015) | Córdoba FMs |
| PERFIL26 | perfil.com Córdoba ratings, Oct 2026 | Córdoba station names |
| STREEMA | streema.com listings | Cross-check |
| APP / CONCLUSION | App Store listings; conclusion.com.ar | Rosario stations |
| LACAPITAL | lacapitalmdp.com (2018) | Radio Provincia AM 1270 / FM 97.1 |
| UNLP | perio.unlp.edu.ar listening survey | Radio Universidad La Plata |
| MWINFO | mediumwave.info, 2025 | AM 570/650 changes |
| RNSITE | radionacional.com.ar homepage | Network station names only (the site publishes no frequency list) |

**Notes and conflicts:**

- Lat/lon are approximate city centres. The radius is 60 km for FM and 150 km for AM.
- Rows the broadcast research itself marked "unverified" were **dropped**: CABA 101.9 and 107.3, several Radio 10 and Mitre repeaters in Córdoba, Rosario and Mar del Plata, FM Vida Rosario, and Radio Universidad MdP.
- An optional block of 34 small-town or Wikipedia-only Radio Nacional rows was also left out.
- Conflicts kept, with notes:
  - The AM 950 name is in flux (CNN Radio until Dec 2025, then StreAM 950).
  - Which of AM 570 and AM 650 carries the "Radio Belgrano" name is uncertain.
  - LRA23 San Juan FM: 101.7 (ENACOM25) vs 101.9 (HCDN24/eswiki).
  - LRA13 Bahía Blanca FM: 95.1 vs 99.3.
  - Cosquín Rock: 95.5 vs 90.3.
- Missing from the ENACOM 2025 list and backed only by listings and news: Pop 101.5, Radio One 103.7, Late 93.1, DSports 103.1, Radio Jai 96.3.

**Not found:** LV2 Córdoba in the 2025 list, Mitre Córdoba's FM frequency, Vorterix Rosario (102.1 vs 104.1), Mitre Salta and Mitre Ushuaia. Radio Colonia 550 (Uruguay, heard in Buenos Aires) is out of scope.
