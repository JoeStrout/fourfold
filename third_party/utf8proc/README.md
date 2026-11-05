# utf8proc

Unicode normalization and character data (C). Version 2.12.0, from
https://github.com/JuliaStrings/utf8proc (tag `v2.12.0`): `utf8proc.c`,
`utf8proc.h`, and the data tables `utf8proc_data.c` (which `utf8proc.c`
includes). MIT license (and the Unicode data license), in `LICENSE.md`.

Fourfold uses it for NFC normalization of submitted form text (M7), behind
base's `utf8.h`.
