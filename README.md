# uavs3e
 uavs3e is an opensource avs3 encoder, supports AVS3-P2 baseline profile. <br>
 features:
 1) AVS3-P2 baseline profile.
 2) supports to compile for Windows/Linux systems.
 3) optimized for SSE4/AVX2 chips.
 4) 10bit encoding on all supported platforms (without SIMD).
 5) The uavs3 codec has supported x86 and arm platforms, and has been tested and verified on the Kunpeng processor. 
 6) The ARM platform recommends the Kunpeng processor.

# license
  Copyright reserved by “Peking University Shenzhen Graduate School”, “Peng Cheng Laboratory”, and “Guangdong Bohua UHD Innovation Corporation” <br><br>
  This program is a free software. You can redistribute it and/or modify it under the terms of the BSD 4-clause license. <br>
  For more details, please view the file "COPYING" in the project.
  
# compile
  The default configuration only support 8bit encoding. <br>
  To support 10bit streams encoding: cmake -DCOMPILE_10BIT=1 

## windows
Prerequisites:
  Visual Studio 2017

build:
  1. ./version.bat (to generate version.h)
  2. solution file: build/x86_windows/uavs3e.sln 
  
  To support 10bit streams encoding, edit inc/com_api.h : #define COMPILE_10BIT 1 

## linux
Prerequisites:
  1. gawk (http://www.gnu.org/software/gawk/)
  2. CMake (https://cmake.org) version 2.8 or higher
  
Build:
  1. mkdir build/linux
  2. cd build/linux && cmake -DCOMPILE_10BIT=0 ../..
  3. make && make install
  
  To support 10bit streams encoding: cmake -DCOMPILE_10BIT=1
  to build shared library, set BUILD_SHARED_LIBS=1 please.

# Run tests
## window/linux

  uavs3e [--config file] [-paramShort ParameterValue] [--paramLong ParameterValue]

     --config file    
             All Parameters are initially taken from the 'file', typically: "encode_RA.cfg".

     -paramShort ParameterValue
     --paramLong ParameterValue
             If -paramShort or --paramLong parameters are present, then the ParameterValue will override the default settings in the configuration file.

# Low-latency chunked bitstream output (optional)

  By default a picture's bitstream is only available in `enc_stat_t.buf/bytes`
  after the whole picture is encoded. Setting `output_chunk` (and optionally
  `output_chunk_opaque`) at the end of `enc_cfg_t` enables streaming output:
  the encoder calls the callback on its encoding worker thread as soon as a
  prefix of the current picture's bitstream has become stable, i.e. while
  later CTU rows are still waiting for mode search.

  * The chunks of one picture, concatenated in call order, are byte-identical
    to the `enc_stat_t.buf/bytes` returned for that picture: no byte is
    repeated, dropped, or modified after delivery.
  * `completed_rows` is the number of CTU rows that have finished the real
    entropy write; it never decreases within one picture.
  * `final` is nonzero exactly once per picture, on the last chunk; that call
    may carry zero bytes.
  * The callback runs synchronously on an encoder worker thread and must not
    re-enter the encoder.
  * Borrow period: `data` is only borrowed for the duration of the callback.
    Consume or copy it inside the call; the encoder may reuse the storage
    immediately after the callback returns.

  Configuration scope: chunked output requires 8-bit I420, fixed QP
  (`rc_type == RC_TYPE_NULL`), `max_b_frames == 0`, `frm_threads == 1`,
  `wpp_threads == 1`, `adaptive_dqp == 0`, `alf_enable == 0`,
  `sao_enable == 0` and `use_pic_sign == 0` (deblocking may stay enabled).
  If `output_chunk` is set with any other configuration, `uavs3e_create`
  returns NULL with `err == COM_ERR_UNSUPPORTED`; without the callback every
  previously valid configuration keeps working unchanged. See
  `examples/chunk_output.c` for a minimal sender that writes the chunks to a
  file in order.


# Contributors
This program is originally developed by the team of Prof.Ronggang Wang (rgwang@pkusz.edu.cn) at Peking University Shenzhen Graduate School. <br>

* Main contributors:  
  * Zhenyu Wang (wangzhenyu@pkusz.edu.cn), Peking University Shenzhen Graduate School. 
  * Bingjie Han (hanbj@pkusz.edu.cn), Peking University Shenzhen Graduate School. 
  * Jiang Du, Peking University Shenzhen Graduate School. 
  * Kui Fan, Peking University Shenzhen Graduate School. 
  * Xi Xie, Peking University Shenzhen Graduate School. 
  * Shengyuan Wu, Peking University Shenzhen Graduate School. 
  * Tong Wu, Peking University Shenzhen Graduate School. 
  * Shiyi Liu, Peking University Shenzhen Graduate School. 
  * Jiayu Yang, Peking University Shenzhen Graduate School. 
  * Chunhui Yang, Peking University Shenzhen Graduate School.
  * Dapeng Chen, Bigo Technology Pte. Ltd 
  * Jin Lin, Harbin Institute of Technology, Shenzhen.
  * Yi Ma, Peking University Shenzhen Graduate School. 
  * Litian Li, Peking University Shenzhen Graduate School. 
  * Guisen Xu, Peking University Shenzhen Graduate School. 
  * Xufeng Li, Peking University Shenzhen Graduate School. 
  * Yangang Cai, Peking University Shenzhen Graduate School. 
  * Hao Lv, Peng Cheng Laboratory. 
  * Ronggang Wang (rgwang@pkusz.edu.cn), Peking University Shenzhen Graduate School

  # XueliCheng
  # luofl

# References
*This program also refers to the following fast algorithm:
  * 陈焕浜, 杨海涛. SMVD编码加速. AVS技术提案, M5218. 2020年3月.
  * 赵寅, 杨海涛. 块划分快速算法. AVS技术提案, M5137. 2020年3月.
  * 张焕宸, 陈雅梅, 喻莉. 基于灰度共生矩阵与编码信息的帧内预测快速算法. AVS技术提案, M5327.
