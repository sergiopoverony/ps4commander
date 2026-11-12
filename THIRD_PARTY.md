# Third-party code

## libsmb2
SMB2/3 client library, `third_party/libsmb2/` (unmodified sources of the files used, plus a hand-written `config.h`).
Upstream: https://github.com/sahlberg/libsmb2 (commit in `third_party/libsmb2/VERSION.txt`).
License: GNU LGPL v2.1 (`third_party/libsmb2/LICENCE-LGPL-2.1.txt`). The library is linked statically; the complete
source of the library is included in this repository, so it can be modified and relinked.
