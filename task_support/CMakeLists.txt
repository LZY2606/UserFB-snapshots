cmake_minimum_required(VERSION 3.16)
project(task_xvid C)
add_library(taskxvid STATIC
  ../xvidcore/src/decoder.c
  ../xvidcore/src/encoder.c
  ../xvidcore/src/xvid.c
  ../xvidcore/src/bitstream/bitstream.c
  ../xvidcore/src/bitstream/cbp.c
  ../xvidcore/src/bitstream/mbcoding.c
  ../xvidcore/src/dct/fdct.c
  ../xvidcore/src/dct/idct.c
  ../xvidcore/src/dct/simple_idct.c
  ../xvidcore/src/image/colorspace.c
  ../xvidcore/src/image/image.c
  ../xvidcore/src/image/interpolate8x8.c
  ../xvidcore/src/image/font.c
  ../xvidcore/src/image/postprocessing.c
  ../xvidcore/src/image/qpel.c
  ../xvidcore/src/image/reduced.c
  ../xvidcore/src/motion/estimation_bvop.c
  ../xvidcore/src/motion/estimation_common.c
  ../xvidcore/src/motion/estimation_gmc.c
  ../xvidcore/src/motion/estimation_pvop.c
  ../xvidcore/src/motion/estimation_rd_based.c
  ../xvidcore/src/motion/estimation_rd_based_bvop.c
  ../xvidcore/src/motion/gmc.c
  ../xvidcore/src/motion/motion_comp.c
  ../xvidcore/src/motion/vop_type_decision.c
  ../xvidcore/src/motion/sad.c
  ../xvidcore/src/prediction/mbprediction.c
  ../xvidcore/src/plugins/plugin_single.c
  ../xvidcore/src/plugins/plugin_2pass1.c
  ../xvidcore/src/plugins/plugin_2pass2.c
  ../xvidcore/src/plugins/plugin_lumimasking.c
  ../xvidcore/src/plugins/plugin_dump.c
  ../xvidcore/src/plugins/plugin_psnr.c
  ../xvidcore/src/plugins/plugin_ssim.c
  ../xvidcore/src/plugins/plugin_psnrhvsm.c
  ../xvidcore/src/quant/quant_h263.c
  ../xvidcore/src/quant/quant_matrix.c
  ../xvidcore/src/quant/quant_mpeg.c
  ../xvidcore/src/utils/emms.c
  ../xvidcore/src/utils/mbtransquant.c
  ../xvidcore/src/utils/mem_align.c
  ../xvidcore/src/utils/mem_transfer.c
  ../xvidcore/src/utils/timer.c
)
target_compile_definitions(taskxvid PRIVATE ARCH_IS_GENERIC ARCH_IS_64BIT ARCH_IS_LITTLE_ENDIAN __XVIDCORE__)
target_compile_options(taskxvid PRIVATE -O2 -Wno-shift-negative-value)
target_link_libraries(taskxvid PUBLIC m)
