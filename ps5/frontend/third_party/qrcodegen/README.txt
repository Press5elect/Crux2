QR Code generator library (C), by Project Nayuki - MIT License (LICENSE; the files' own headers
carry it too).

  https://github.com/nayuki/QR-Code-generator, revision 3c6d0b3cefb4e049dc337e82237c9644399716a8
  (2026-08-31, the newest when it was taken on 2026-10-11): c/qrcodegen.c and c/qrcodegen.h,
  unmodified.

Crux2 draws the web panel's address as a QR code with it (ps5_web.cpp, PS5Web::qr). The C
version is used rather than the C++ one PSSwanStation has, because the C++ one throws
exceptions and the title may be built with -fno-exceptions. qrcodegen.c checks its arguments
with assert(): build it with -DNDEBUG on the console (no system module exports __assert).
