自建AC-3回归素材，非外部录音。fixture_gen.c及生成的音频按CC0-1.0提供。
任务运行直接读取固定AC-3文件，不调用生成器或FFmpeg。
输入为48kHz、12×1536帧的s16le，不同声道低幅信号；准备时以FFmpeg ac3、448k、5.1(side)/5.0(side)编码。
[
  {
    "name": "surround_lfe.ac3",
    "channels": 6,
    "layout": "5.1(side)",
    "sha256": "4fb6ec07c9c0862cc6c2f71da24e851bd8902e9cc92c596f534e12a31f033d6d",
    "bytes": 21504,
    "pcm_sha256": "5839ab31eb05359bd4cee401f9505324085ca8e673ce7a7ce302712bad0b4af0"
  },
  {
    "name": "surround.ac3",
    "channels": 5,
    "layout": "5.0(side)",
    "sha256": "e2ab5198e5490266ccb97445c935aaf49794041165bd1c162919cd48790a171d",
    "bytes": 21504,
    "pcm_sha256": "f4421ce03e2096cd6ea8f8e84810ccca9d4e631aff3daa8989e1cfd587333548"
  }
]
