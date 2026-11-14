RFB 桌面扩展的离线字段说明

上游规范：https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst （SetDesktopSize、ExtendedDesktopSize、Screen Model）。此文件为字段摘要，不包含实现。

* SetDesktopSize：U8=251、U8 padding、U16 width、U16 height、U8 screen count、U8 padding，然后 count 个 Screen。整数均为大端。
* Screen：U32 id、U16 x、U16 y、U16 width、U16 height、U32 flags，共16字节。
* ExtendedDesktopSize：矩形 encoding=-308，header 的 x 为 reason、y 为 result、width/height 为 framebuffer 尺寸；正文 U8 count、3字节padding、count 个 Screen。
* reason 0 是服务端变化，1 是自己的请求答复，2 是其他客户端变化；未知 reason 按0处理。只有 reason=1 时 result 有意义。result=0 成功，非0是未确认成功（包括4=forwarded）；失败的其他字段未定义，仍必须完整读完其结构，不按几何约束解释这些字段。后续服务端变化才是新尺寸的确认。
* 屏幕布局成功或发请求时：framebuffer与screen尺寸非零，屏幕数1..255，ID唯一，屏幕不得超出framebuffer；计算边界不可在uint16中溢出。本需求允许屏幕重叠或留空。未知flags原样保留。
* 支持扩展的证据是收到其完整矩形，仅声明SetEncodings不够。错误的整条FramebufferUpdate不得发布支持或布局状态。
* 同条更新允许多个扩展矩形，按线缆顺序处理；不能包含像素矩形。本需求同时拒绝同条消息混入旧DesktopSize，以消除两种拓扑定义混用。结构错误直接返回错误，不要求错误后重新同步已损坏的字节流。
* 公共Screen的ID/Flags为uint32，其余为uint16；DesktopLayout的Width/Height为uint16，Screens为[]Screen。ExtendedDesktopSizePseudoEncoding结果公开Reason/Result(uint16)和Layout(DesktopLayout)。
