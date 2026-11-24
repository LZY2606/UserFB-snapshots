这些是本地编写的合法离线调试样本，按根目录LICENSE的BSD-3-Clause条款提供。alpha.cc和beta.cc分别编译成两个CU，observe.cc仅提供编译器内存屏障，避免局部变量全部消失。不运行生成的ELF。

已附的optimized4.elf和optimized5.elf为ELF64 little-endian x86-64 ET_EXEC，分别含DWARF4和DWARF5，来自-O2优化，链接后无待应用的重定位。普通构建不需要目标平台工具链。可选重建命令：ELF_LD=/path/to/ld.lld CLANGXX=/path/to/clang++ bash samples/src/build.sh。生成时使用Apple clang 21.0.0和LLD 21.1.8；版本详情由作者证据单独保存。脚本不依赖C/C++运行时或系统头文件，路径映射为源码根相对路径。查询使用提供的二进制，换编译器后布局可能不同。
