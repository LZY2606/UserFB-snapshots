alternate.ogg is an created 733 Hz synthetic sine, not a recording of a person or a third-party asset. It is provided under CC0-1.0.

Created once with FFmpeg 8.1.2 (native experimental Vorbis encoder):

```sh
ffmpeg -hide_banner -nostdin -y -f lavfi -i 'sine=frequency=733:sample_rate=44100:duration=0.16' -ac 2 -c:a vorbis -strict experimental -q:a 3 -map_metadata -1 alternate.ogg
```

No FFmpeg binary or source is shipped or needed at runtime. The checked-in file is the input; tests must not regenerate it. SHA256: 99effd7e1ea48c5d39a27332cc7dd42790598c5adc7a9c04bf2ffdcbf672954b

The existing Go decoder reads 7104 stereo frames at 44100 Hz from this file. Its setup packet differs from testdata/eof_issue.ogg. The latter fixture is stereo too, but its final page deliberately lacks EOS; see its existing upstream regression. These are compressed inputs, not implementations of chain handling or seek indexing.
