# Vendored nanopb

Runtime sources of **nanopb 0.4.9.2**, used to decode/encode the HiFlow
protobuf messages on the ESP32-C6 (and in the host tests).

| | |
|---|---|
| Upstream | https://github.com/nanopb/nanopb |
| Version  | `nanopb-0.4.9.2` (`NANOPB_VERSION` in `pb.h`) |
| Git tag  | `nanopb-0.4.9.2` |
| Commit   | `160d4f09e5fabb2b66aa2dea32d4f38ace2c4b3f` |
| License  | zlib (see `LICENSE.txt`) |

## What is vendored

Only the files needed to build with **static allocation** (no dynamic memory,
which is what the embedded target uses):

```
pb.h            pb_common.c   pb_common.h
pb_decode.c     pb_decode.h
pb_encode.c     pb_encode.h
LICENSE.txt
```

The `pb_*.c` files are compiled unmodified.

## Refreshing / provenance check

```sh
sha256sum pb.h pb_common.c pb_common.h pb_decode.c pb_decode.h pb_encode.c pb_encode.h
```

Expected digests for 0.4.9.2:

```
e0db84a27e0d41a2d2d347b8c879e30ceb856d36dc192cce0f1124f833c67bc2  pb.h
8d2ec28baaaf2b7a5e90e4cb2fa9700d21cef7f826f051a637c30b7a1e6a0516  pb_common.c
6495a691aca68d6973f2274b5dd54b74fbb57f6b019c45fff255a857fe1abcfd  pb_common.h
f5b425beaa207251e531c8ce2c86c9b6867e2920ed59cc1b125332af0c147632  pb_decode.c
fcac5f7680fe6e870157e4bcf34d5162bdd4fff0d7db3cad1122f2ad24a6da87  pb_decode.h
debb0714dff8d1515b9724eae45531cad912f12028a0311fc3e2c694689e1fed  pb_encode.c
9aa00fee4ff08adf0da16e33a55be08810ea657800a648dc78f82e89c60c10cf  pb_encode.h
```

To re-vendor:

```sh
git clone https://github.com/nanopb/nanopb.git /tmp/nanopb-src
git -C /tmp/nanopb-src checkout nanopb-0.4.9.2
cp /tmp/nanopb-src/{pb.h,pb_common.c,pb_common.h,pb_decode.c,pb_decode.h,pb_encode.c,pb_encode.h,LICENSE.txt} .
```

The decoder/encoder sources in `../generated/` were produced with the generator
from this same 0.4.9.2 checkout — see `../generated/README.md`.
