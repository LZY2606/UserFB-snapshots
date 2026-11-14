本任务的互通格式参考 draft-holmer-rmcat-transport-wide-cc-extensions-01：
https://datatracker.ietf.org/doc/html/draft-holmer-rmcat-transport-wide-cc-extensions-01
该文档是已归档草案，不将其称作正式 RFC。这里只要求下列固定子集。

RTP 扩展 profile 0xBEDE：one-byte 元素高四位为 ID，低四位加一为数据长度；ID=0 是单字节填充，ID=15 结束扫描。目标 ID 数据长度须为2，内容是网络序 transport sequence；一份扩展里有多个目标项时视为无有效记录。扫描遇到越界项时本包不记账。合法的未知项跳过。

RTCP：V=2/P=0/FMT=15/PT=205。sender SSRC 取调用流本地 SSRC，media SSRC=0；其后为16位 base sequence、16位 status count、24位 reference time、8位 feedback count。reference time 以64000微秒为单位，使用本包首个已接收序号的到达时间向下取整后低24位。反馈计数从0开始，每次成功发送加一并按8位回绕。

status symbol 0/1/2 分别表示丢失、小增量、大增量；3不用。状态块均16位：最高位0是RLE，接着2位symbol与13位run length；最高位1是向量，下一位0装14个1位symbol，下一位1装7个2位symbol。末块不足的位补0。允许任选合法状态块组合，不要求最短编码；max_bytes只能放下整个状态块与对应增量时才能纳入对应序号。

按序号顺序写接收增量，丢失不占增量。第一个接收包相对reference time，其后相对前一个已编码接收包；先把各到达时间换成250微秒tick（向下取整），再求差。0..255用symbol1和一个无符号字节，其余可表示的差用symbol2和网络序有符号16位。差超出范围时在此前结束报告，下次为剩余区间重新建立reference time。RTCP尾部用0补齐4字节，length为32位字数减1。没有可报告状态或尚未配置时返回RTP_NOT_FOUND且不发数据；配置无效、预算不足返回RTP_INVALID_VALUE；真实发送失败保留原错误。配置同ID重复调用须幂等；仅已配置调用流可触发反馈。
