# AI use in this project

AI writes code here. This says how, and what is done so that the result is
still engineering.

## The rules

**Nothing merges on an assistant's say-so.** Every change passes
`bench/verify.ps1` — a host test suite, a C++ header check, and a firmware
build for every board — and the runner executes that itself rather than
trusting a report. What an assistant believes it did decides nothing.

**A green gate is not a working device.** Three labels, used literally:

| | |
|---|---|
| **Hardware-verified** | observed on a named board and recorded |
| **Build-verified** | compiles, tests pass, nothing has run on hardware |
| **Implemented** | the code exists |

The gate can only produce Build-verified. Every serious defect here reached
Build-verified first: a struct tag valid in C and fatal in C++, a build glob
evaluated once so new sources were never compiled, a crash reporter that
faulted parsing its own coredump and put a board in a boot loop.

**Behaviour claims are measured or they are not made.** Numbers in comments
came from a measurement. `pocsag.c` records 92% codeword failure before a
sampler fix and zero after, because that is what was counted. A CRC is checked
against a reference implementation, not against itself.

**Nothing is claimed that the code does not do.** The P25 app once displayed
`TRUNK MONITOR` with no trunking behind it. That is the failure this project
guards hardest against — in the UI, in the README, and here.

## What this is not

Not vibes. Not a chat log committed as documentation. Not a repository of
prompt files pretending to be design.

Working notes, task queues and assistant configuration are **deliberately not
in this repository**. They are scaffolding, and scaffolding left standing is
mistaken for the building. `bench/` holds the test bench and the verification
gate — things that run and can fail. What an assistant was told, and which
assistant it was, is local configuration and stays local.

If a document here does not say something a person needs in order to use or
change the firmware, it should not exist.

## Provenance

Every commit is authored by the maintainer, who is responsible for it. There is
no separate class of "AI commits" to be discounted or excused; if it is in the
history, someone stands behind it.

Vendored code keeps its origin and its licence. `components/lakeshark/apps/p25/`
descends from DSD and mbelib and is kept close to upstream on purpose, so it can
be followed rather than quietly diverged from. Reverse-engineered protocol
details record how each number was obtained — measured off air, computed from
registers, or confirmed out of sample — because a specification that cannot say
where it came from cannot be checked by anyone else.

## Licence

See `LICENSE`. Using this work is welcome; passing it off is not. If something
here is useful, take it and say where it came from.
