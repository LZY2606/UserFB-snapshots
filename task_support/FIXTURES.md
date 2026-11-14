tone.aac: author-generated two-sine PCM, encoded with ffmpeg AAC-LC, 44100 Hz mono, 112 kbit/s. Preparation command is in private evidence; ffmpeg is not needed at runtime.
single.loas: first raw AU from tone.aac, wrapped as version-0 single-program single-layer LATM, one subframe and fixed ASC 0x1208. A basic input fixture, not a transport parser or decoder implementation.
