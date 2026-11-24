这些是只读合成 PE32+ 数据，不含可执行的有效程序。fixtures 包只编码字节，可用 Embedded("NB09") 或 Embedded("NB11") 生成内嵌数据，NB10() 生成已有外部引用。新回归还需自行构造独立数据，不能只重复这份素材。数据布局参考 Wine include/wine/mscvpdb.h 的 OMF 结构及 Microsoft 的 CodeView 目录命名；所有整数为小端。

生产接口约定：在现有 *DebugCodeView 上增加 Embedded *EmbeddedCodeView。NB09/NB11 时非 nil；NB10 为 nil，原 CodeViewInfo 的 Signature/Offset/Date/Age/PDBPath 含义不变。内嵌记录保留真实 Signature 与目录 Offset，Date 为 time.Time{}、Age 为0、PDBPath 为空，不能把 OMF 字节解释成外部身份。EmbeddedCodeView 的公开结构可自行设计，但须能枚举模块及原始目录身份。在 *DebugCodeView 增加 SourceLines(rva uint32) []CodeViewSourceLine。CodeViewSourceLine 固定字段：Module uint16；ModuleName、File string；Line uint16；StartRVA、EndRVA、SubsectionOffset、FileOffset、LineOffset uint32。后三个偏移均相对当前 CodeView 载荷起点，LineOffset 指向提供该行的 offset 数组元素。结果按 Module、SubsectionOffset、FileOffset、LineOffset 排序。不同模块/文件/同址行不可合并。查询不改输入或调试对象；未覆盖返回空，重复/逆序查询一致。

本次支持单个 OMF 目录，lfoNextDir=0。前8字节是签名字节和目录偏移 u32；目录头依次为 cbDirHeader u16、cbDirEntry u16、cDir u32、lfoNextDir u32、flags u32，按头与条目声明步长前进，允许尾部扩展。每项前12字节是 SubSection u16、iMod u16、lfo u32、cb u32，lfo 相对载荷开头。条目可乱序，同模块的不同种类子节不要求相邻。识别 sstModule=0x120、sstSrcModule=0x127、sstSegMap=0x12d；其他子节允许跳过。目录和已识别子节的引用均须在对应载荷/子节范围内，截断返回带子节偏移的 error，不 panic；非零 nextDir 或本范围以外的分段模式可明确报不支持。

sstModule 以 iMod 为模块身份；头8字节为 ovlNumber、iLib、cSeg 各 u16 和 Style[2]。随后 cSeg 个12字节贡献：Seg u16、pad u16、Off u32、cbSeg u32；最后 u8 长度加模块名字节。贡献的逻辑段范围是 [Off,Off+cbSeg)。本次只处理 ovlNumber=0、Style="CV"。

sstSegMap 为全局表，iMod=0xffff。头 cSeg/cSegLog 各 u16，本次二者相等。描述符每项20字节：flags、ovl、group、frame、iSegName、iClassName 各 u16，offset/cbSeg 各 u32。数组位置+1是逻辑段号；frame 是1起始 PE 物理节序号，二者可能不同。支持 ovl=group=0、flags=0x0109 的平坦32位条目。逻辑段偏移 x 映射到 SectHdrs[frame-1].RelAddr+offset+x；x 必须落在 [0,cbSeg)，映射后还必须处于该 PE 节的 VirtualSize 范围。全程使用 RVA，不叠加 ImageBase；用宽整数检查端点再窄化，不能回绕。

sstSrcModule 头 cFile/cSeg 各 u16，随后 baseSrcFile[cFile] u32、每段 start/end 成对的 u32 数组、seg[cSeg] u16。每个 baseSrcFile 指向一个源文件记录：cSeg/reserved 各 u16、baseSrcLn[cSeg] u32、每段 start/end 的 u32 数组，最后 u8 长度和文件名。baseSrcFile 与 baseSrcLn 都相对此 sstSrcModule 开头，不相对源文件、目录或 PE 文件。文件和行表可分散存放，不允许用顺序游标替代这些引用。每个源行表是 Seg/cLnOff 各 u16、offset[cLnOff] u32、lineNbr[cLnOff] u16；offset 非降序，重复地址可表示多个源行。其 Seg 与源模块段号关联，而非与数组下标关联。

源模块和源文件的 start/end 是闭区间。行从自身 offset 覆盖到下一个严格更大的 offset；最后一行到本文件段 end+1。同址多行使用同一右界。实际区间还要同时裁剪到对应源模块段、源文件段、同模块同逻辑段的每条贡献、段映射有效范围和 PE 节范围；空交集不输出，不跨空洞/模块借用前驱。一个行被不同贡献裁成多段时每段保留。超出贡献但仍有合法源行的记录不会扩张贡献。NB10、FPO 和 Misc 原生产读取继续兼容，不增加外部 PDB、内联、符号服务器或动态装载支持。

fixtures 的目录位于载荷0x300，调试载荷位于 PE 文件0xd00。模块7使用逻辑段2映射到物理节1+0x30；模块9使用逻辑段1映射到物理节2+0x80。其余字段直接见构造器；它不包含查询实现或预生成查询结果。
