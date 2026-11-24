# Fixed libRIST runtime export

This tree is a source subset of https://github.com/irlserver/librist at commit e1da2519fae898daad9606db0914e9bbd0b5bd31. Upstream files are copied byte for byte; UPSTREAM_SOURCES.json lists every delivered file and its origin and hash. COPYING and dependency source license notices are retained.

Use `bash task_support/run.sh baseline` to reproduce the original fanout starvation. After implementing the requested C changes and C regression, use `bash task_support/run.sh target`. The supplied build uses a system C compiler, ar, bash, POSIX threads and libm, with bundled cJSON, LZ4 and base crypto sources. It compiles all eligible src/*.c files, including newly added production C files, and selects tun_darwin.c or tun_linux.c.

The existing build configuration sets HAVE_MBEDTLS=0, HAVE_NETTLE=0 and HAVE_SRP_SUPPORT=0. The optional contrib/mbedtls tree is not delivered; it is not a compile dependency of this configuration or the requested per-receiver media retransmission behavior. Optional SRP translation units remain upstream source but are not compiled. TLS/mbedTLS/SRP functionality has not been tested. The Meson files are retained as upstream reference; the supported build entry is task_support/run.sh, not a full Meson distribution.

Unrelated tools, CI, optional third-party trees and other upstream test programs are not part of this runtime export. All src sources, public/internal headers and templates, compatibility files, protocol docs and the existing retransmission sequence-domain test are retained. No production source or retry state has been changed in this export.
