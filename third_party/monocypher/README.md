# Monocypher

Small cryptography library (C). Version 4.0.2, from
https://monocypher.org/download/monocypher-4.0.2.tar.gz: `src/monocypher.c`
and `src/monocypher.h`, with `LICENCE.md`. CC0 or 2-clause BSD, at our choice.

Fourfold uses BLAKE2b (plain and keyed) and the constant-time comparisons,
through `base/crypto.h`. The optional Ed25519 files aren't vendored.
