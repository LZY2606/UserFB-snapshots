# Opus 录音接口约定

新增接口仍在 minimp4.h，保持单头文件用法；task_support/implementation.c 负责实例化生产实现。

```c
int MP4E_add_opus_track(MP4E_mux_t *mux, const void *head, int head_bytes);
int MP4E_put_opus_packet(MP4E_mux_t *mux, int track, const void *packet, int bytes);
int MP4E_finish_opus(MP4E_mux_t *mux, int track, uint32_t end_trim);
```

add 成功返回零基轨号，另两项成功返回 MP4E_STATUS_OK；错误使用现有负状态码。输入 head 是完整 OpusHead。只接受 version=1、mapping family=0 或255；family0严格19字节且输出声道1或2；family255严格21+输出声道数个字节。输出声道、流数非零，coupled<=streams，streams+coupled<=255，每个映射值小于 streams+coupled 或为255；允许重复和255静音映射。首跳、输入采样率、增益沿用 OpusHead 的小端字段含义。库持有配置副本，调用方可立即覆盖原缓冲。语言使用und。其它family、错误长度和fragmentation模式都拒绝，不能遗留半个轨道。

put 的一次输入是一整份多流样本：前 streams-1 流采用 RFC6716 Appendix B 的 self-delimited framing，最后一流采用普通 framing。每流都要按 TOC code0/1/2/3 验证帧数、长度、VBR/CBR、padding，不能搜索相似TOC来猜边界。允许合法零字节frame，不验证熵编码负载；最大单帧1275字节、总时长至多5760个48k采样帧。各流时长必须相同，TOC的单/双声道编码模式允许独立切换，不据此重写声道映射；尾流必须恰好消费所有剩余字节。样本原字节保留，一个输入包就是一个MP4样本，stts时长取任一流的真实48k时长，不能乘输出声道数。拒绝发生在写出数据或排空sequential前驱之前；之后仍可提交合法包。累计原始时长限制为UINT32_MAX。旧 MP4E_put_sample 不得向 Opus 轨提交或续片。

finish 用每声道48k帧数给出尾裁；要求已经有包、end_trim小于末包完整时长，且累计时长大于等于head.pre_skip+end_trim。失败不封口；成功后该轨不能追加或重复finish，其他轨照常写。close发现未finish的Opus轨返回错误并释放资源，不生成最终moov。回调错误沿用原库约定，不承诺撤回外部writer已经接收的内容。

非分片普通和sequential都写真实Opus AudioSampleEntry和唯一dOps，不写mp4a/esds。dOps为version0，把配置多字节字段转换为大端，PreSkip保留传入值；sample entry channelcount用streams+coupled，而dOps.OutputChannelCount用映射的输出声道数，采样率固定48000。兼容brand含iso2，Opus全部样本为同步样本，不生成stss。mdhd/stts保留完整包时长，不把首跳或尾裁塞进包duration。含Opus的文件使用48000 movie timescale，每条Opus轨写一个rate=1的elst，media_time=head.pre_skip，segment_duration=累计原始时长-pre_skip-end_trim；tkhd用可听时长，mvhd用各轨显示时长的最大值，其他轨按自己的时基换算。无Opus时旧文件字节保持。

roll用sgpd version1/default_length=2和sbgp version0表达，组类型均为roll，所有样本都要映射。对零基样本i，在此前的包中从近到远取刚好覆盖3840帧的最少k包，roll_distance=-k；此前全部包不足3840时用-(i+1)，读取器可把负起点夹到轨首。相邻相同距离合并游程，重复距离复用描述项；不能固定写-4。roll计数使用完整包时长，不使用裁剪后的显示时长。

MP4D_track_t 新增 unsigned sample_entry、opus_trim_start、opus_trim_end；Opus轨的sample_entry为0x4f707573，dsi/dsi_bytes给出重建的version1 OpusHead，由MP4D_close释放。新增trim字段分别返回elst首跳和原始总时长减去首跳及显示时长；MP4D_frame_offset继续给原始包范围、未裁剪DTS和完整duration。此次只读单描述Opus轨、48000 movie/media时基、单条rate1且非负media_time的elst；缺失/重复dOps、坏版本/映射、截断、缺失或无法表示的裁剪均使open失败并清理。elst在mdhd/stts之前也必须正确，未知子盒允许跳过。旧音视频轨dsi含义不变，原有时间单位也不变。

标准材料是本目录的RFC6716（§3和Appendix B）、RFC7845（§5.1）。容器语法参考 https://opus-codec.org/docs/opus_in_isobmff.html ，上述显式接口与取值范围是本次支持子集。此处不要求音频解码、Ogg解析、fragmented MP4、超过32位的音频总时长或更改既有编解码器。
