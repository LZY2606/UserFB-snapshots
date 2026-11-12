set -euo pipefail
cmake -S . -B "$1" -DCRYPTO_LIBRARY=internal -DENABLE_WARNINGS_AS_ERRORS=OFF -DLIBSRTP_TEST_APPS=ON -DCMAKE_DISABLE_FIND_PACKAGE_PCAP=TRUE
cmake --build "$1" --target srtp3 rdbx_driver replay_driver roc_driver srtp_driver -j 4
