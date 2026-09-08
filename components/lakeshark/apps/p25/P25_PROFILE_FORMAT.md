# LakeShark P25 profile format, version 1

Profiles are UTF-8-compatible, line-oriented text.  Each data line is
`key=value`; blank lines and lines whose first non-space character is `#` or
`;` are comments.  Keys and symbolic values are case-sensitive.  CRLF and LF
are accepted.  Decimal integers contain digits only: frequencies are integer
hertz, never floating-point MHz.

Required singleton fields are `version=1`, `system`, and `site`.  At least one
`control` frequency is required.  Optional fields and their defaults are:

| Field | Values | Default |
|---|---|---|
| `preferred` | a frequency present in the control list | first control |
| `auto_follow` | `true` or `false` | `true` |
| `encrypted_skip` | `true` or `false` | `true` |
| `encrypted_skip_ms` | unsigned 32-bit decimal milliseconds | `30000` |
| `demod` | `auto`, `c4fm`, `cqpsk`, `diff_4fsk`, `fsk4_tracking` | `auto` |
| `cqpsk_timing_gain` | decimal `0.0000390625` .. `0.000625` | `0.00015625` |
| `cqpsk_carrier_gain` | decimal `0.0025` .. `0.04` | `0.01` |

`control=<hz>` may occur up to 16 times.  Frequencies are checked against the
radio endpoint ranges supplied by the caller.  `tg=<id>|<alias>|<enabled>|<priority>`
may occur up to 64 times.  A TG ID is 1..65535, `enabled` is `true` or `false`,
and priority is the scan controller's 0..255 rank (zero means ordinary).
Aliases use the scan controller's existing 24-byte storage, including the NUL.
At most 16 TGs may have nonzero priority, matching scan controller capacity.

Singleton fields, control frequencies, and TG IDs must be unique.  Duplicates
are rejected rather than using first-wins or last-wins.  Unknown keys, partial
TG rows, oversized input, and any validation error reject the whole profile;
the caller's previous destination remains unchanged.

The CQPSK values are coefficients in LakeShark's recovered679 loops: timing
changes estimated samples/symbol per unit Gardner error, while carrier changes
the residual differential phase estimate per unit phase error. Higher values
acquire faster but follow noise more readily. CONFIG reset restores the
fixture-tested defaults above. Similarly named gains from another receiver do
not necessarily use these units.

See `tools/p25_profile.example` for fictional data.  Parsing that file only
constructs a model.  It does not retune a radio, change scan policy, write NVS,
or represent settings as applied.

## Where the radio looks for one

`/sdcard/p25_profile.txt` (`P25_PROGRAM_DEFAULT_PATH`). Copy
`tools/p25_profile.example` there and edit it. Nothing is read at boot: the
SD may not be mounted that early, and a slow probe would delay a headless boot
for nothing. The P25 PROGRAM tab's RELOAD button is what reads it, and the tab
shows that path at all times so an operator with no profile - or with one that
will not load - is told where it should be.

Before any profile has been loaded, PROGRAM reads `NO PROFILE LOADED`, with
`-` for the system, the site and the selected control channel. That is the
first-run state, not an error.

A failed reload never disturbs the running profile. The whole file is read,
parsed and validated before one frequency or one policy bit is applied, so a
missing file, an oversized file or a bad line leaves the radio on the system it
was already on and puts the reason under LAST LOAD - `line 7: unknown field`,
`profile file is too large`, `no profile file at that path` - followed by
`(active profile kept)`.

## What a profile applies, and what it does not

Applied: the control channel and the selected control, auto-follow, the
encrypted-skip policy and its duration, the demod preference, and the scan
controller's allow list and priority ranks. The hold is cleared, because a hold
names a talkgroup on the system that was programmed before this one.

Not applied: lockouts, which are the operator's "never again" and outlive a
profile swap; the names table, which `/sdcard/p25_names.csv` still owns; and
the vocoder's encryption mute, which no profile can turn off - `encrypted_skip`
steers the grant follower only.

## The radio does not write profiles

There is no save-from-GUI path. A profile is a text file an operator edits, and
the SD card also holds captures; writing to it from the radio is not worth the
class of mistake it invites. If saving is ever added it has to write a
temporary file beside the target and rename over it, never open the target for
writing directly, and never accept a path outside `/sdcard/p25_profile*.txt`.
