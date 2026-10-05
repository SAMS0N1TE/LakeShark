# AI use in this project

AI writes code here. This says how, and what is done so that the result is
still engineering.


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
