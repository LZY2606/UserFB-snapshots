# uvgRTP

uvgRTP is an *Real-Time Transport Protocol (RTP)* library written in C++ with a focus on simple to use and high-efficiency media delivery over the Internet. It features an intuitive and easy-to-use *Application Programming Interface (API)*, built-in support for transporting *Versatile Video Coding (VVC)*, *High Efficiency Video Coding (HEVC)*, *Advanced Video Coding (AVC)* encoded video and Opus encoded audio. Furthermore, uvgRTP can be used to transport *Visual Volumetric Video-based Coding (V3C)* encoded volumetric video. uvgRTP also supports *End-to-End Encrypted (E2EE)* media delivery using the combination of *Secure RTP (SRTP)* and ZRTP. uvgRTP has been designed to minimize memory operations to reduce its CPU usage and latency.

uvgRTP is licensed under the permissive BSD 2-Clause License. This cross-platform library can be run on both Linux and Windows operating systems. Mac OS is also supported, but the support relies on community contributions. For SRTP/ZRTP support, uvgRTP uses the [Crypto++ library](https://www.cryptopp.com/).

Currently supported specifications:
   * [RFC 3550: RTP: A Transport Protocol for Real-Time Applications](https://datatracker.ietf.org/doc/html/rfc3550)
   * [RFC 3551: RTP Profile for Audio and Video Conferences with Minimal Control](https://datatracker.ietf.org/doc/html/rfc3551)
   * [RFC 6184: RTP Payload Format for H.264 Video](https://datatracker.ietf.org/doc/html/rfc6184)
   * [RFC 7798: RTP Payload Format for High Efficiency Video Coding (HEVC)](https://datatracker.ietf.org/doc/html/rfc7798)
   * [RFC 9328: RTP Payload Format for Versatile Video Coding (VVC)](https://datatracker.ietf.org/doc/html/rfc9328)
   * [RFC 7587: RTP Payload Format for the Opus Speech and Audio Codec](https://datatracker.ietf.org/doc/html/rfc7587)
   * [RFC draft: RTP Payload Format for Visual Volumetric Video-based Coding (V3C)](https://datatracker.ietf.org/doc/draft-ietf-avtcore-rtp-v3c/)
   * [RFC 3711: The Secure Real-time Transport Protocol (SRTP)](https://datatracker.ietf.org/doc/html/rfc3711)
   * [RFC 6189: ZRTP: Media Path Key Agreement for Unicast Secure RTP](https://datatracker.ietf.org/doc/html/rfc6189)

You are welcome to contact us in our Discord if you have questions or comments regarding the library [![Discord](https://img.shields.io/discord/973260924288901140?style=plastic)](https://discord.gg/fZpub7BPUA)

## Notable features

* AVC/HEVC/VVC video streaming, including packetization
* V3C Atlas streaming, including packetization
* Ready support for many formats which don't need packetization, including Opus
* Delivery encryption with SRTP
* Encryption key negotiation with ZRTP
* UDP firewall hole punching
* Transport-wide congestion control (TWCC) receiver feedback generation
* Simple to use API
* Working examples
* Permissive license

## Building and linking

See [BUILDING.md](BUILDING.md) for instructions on how to build and use uvgRTP.

## Learning to use uvgRTP

To get started using uvgRTP, check out [usage](USAGE.md) file which contains a step-by-step tutorial for using uvgRTP. To see uvgRTP in action, the [examples](examples) folder contains many working examples to illustrate how to use uvgRTP in variety of situations. For a more detailed description, check the [documentation](docs/README.md) where you can find info on advanced topics and descriptions of all uvgRTP flags.

Furthermore, we also provide a Doxygen documentation of the [public API](https://ultravideo.github.io/uvgRTP/html/index.html) on Github.

## Transport-wide congestion control feedback (TWCC)

The receiver can generate transport-wide congestion control feedback, the
RTPFB/FMT=15 packet described by the fixed wire subset in
[`task_support/twcc-wire.md`](task_support/twcc-wire.md) (based on
draft-holmer-rmcat-transport-wide-cc-extensions-01).

Enable it per receiving media stream with the RFC 8285 one-byte extension id
used by the sender (ids `1`–`14`):

```cpp
stream->configure_twcc(3);
```

The two-byte transport sequence is then read from every matching extension at
the UDP receive point and recorded in one ledger shared by all media streams
multiplexed on the same receive socket. This is what allows audio and video on
one socket to share a single transport sequence space, arrival history and
feedback packet counter. Another stream on the same socket may enable the same
id, but a different id is rejected with `RTP_INVALID_VALUE`. Pass `0` to
`configure_twcc()` to release the stream; when the last user releases, the
shared ledger is cleared. Sockets have independent state. When disabled,
packet reception and media delivery behave exactly as before.

Reports are sent only when the application asks for them:

```cpp
stream->get_rtcp()->send_twcc_feedback(/*max_bytes=*/1200);
```

One call sends one report containing the longest contiguous status prefix that
fits `max_bytes` (the size of the entire RTCP datagram, including header and
padding); call again to send the rest. The reported interval and the 8-bit
feedback count advance only after a successful send. `RTP_NOT_FOUND` is
returned when the calling stream has not configured TWCC or there is nothing to
report, and `RTP_INVALID_VALUE` when the budget is too small.

Scope: this is **plaintext RTP/RTCP only**. It does not perform bandwidth
estimation, sender bitrate control, SDP negotiation or SRTP handling.

## Contributing

We warmly welcome any contributions to the project. If you are considering submitting a pull request, please read [CONTRIBUTING.md](CONTRIBUTING.md) before proceeding.

## Papers

If you use uvgRTP in your research, please cite one of the following papers:

[Open-Source RTP Library for High-Speed 4K HEVC Video Streaming](https://researchportal.tuni.fi/en/publications/open-source-rtp-library-for-high-speed-4k-hevc-video-streaming)

```A. Altonen, J. Räsänen, J. Laitinen, M. Viitanen, and J. Vanne, “Open-source RTP library for high-speed 4K HEVC video streaming,” in Proc. IEEE Int. Workshop on Multimedia Signal Processing, Tampere, Finland, Sept. 2020.```

[UVGRTP 2.0: Open-Source RTP Library For Real-Time VVC/HEVC Streaming](https://researchportal.tuni.fi/en/publications/uvgrtp-20-open-source-rtp-library-for-real-time-vvchevc-streaming)

```A. Altonen, J. Räsänen, A. Mercat, and J. Vanne, “uvgRTP 2.0: open-source RTP library for real-time VVC/HEVC streaming,” in Proc. IEEE Int. Conf. Multimedia Expo, Shenzhen, China, July 2021.```

[Open-source RTP Library for End-to-End Encrypted Real-Time Video Streaming Applications](https://researchportal.tuni.fi/en/publications/open-source-rtp-library-for-end-to-end-encrypted-real-time-video-)

```J. Räsänen, A. Altonen, A. Mercat, and J. Vanne, “Open-source RTP library for end-to-end encrypted real-time video streaming applications,” in Proc. IEEE Int. Symp. Multimedia, Naples, Italy, Nov.-Dec. 2021.```

## Test framework

We also have an easy to use performance test framework for benchmarking uvgRTP against FFmpeg and Live555 on Linux OS. The framework can be found [here](https://github.com/ultravideo/rtp-benchmarks).

## Origin

The original version of uvgRTP is based on Marko Viitanen's [fRTPlib library](https://github.com/fador/fRTPlib).
