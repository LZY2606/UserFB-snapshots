Microformats2 网页抽取子集：隐含属性

来源：https://microformats.org/wiki/microformats2-parsing 的稳定正文
“parsing for implied properties”。该规范注明 CC0；这里是本任务采用的
有限规则说明，不包含实现。数字/厂商类名、dt、旧式兼容和 rel 均不在本次范围。

only-child 按元素孩子计数，文本/注释不计；only-of-type 按同标签元素兄弟计数，
不是按有 href/src 的候选计数。以下包装和候选均不得自身带有效 h-* 根类。
一层包装是根的唯一元素孩子；不允许继续向任意深度搜索。
候选要求的属性必须存在；显式空属性仍算存在。推导值也放入长度一的属性数组。

name：已有任何显式 p-* 或 e-* 属性，或存在任何后代 h-* 根时，不推导。
否则按顺序尝试：根 img/area 的 alt；根 abbr 的 title；根的唯一元素孩子
img/area 的非空 alt 或 abbr 的非空 title；唯一包装的唯一元素孩子上的相同规则。
均未命中则取根文本，略去 script/style/template，img 替换为 alt（缺失则为空），
裁首尾 HTML ASCII 空白。这里的文本 img 不添加空格，也不以 src 替代。

photo：已有任何显式 u-* 属性，或存在任何后代 h-* 根时，不推导。
否则依次尝试根 img[src]、根 object[data]；根的直接孩子中 only-of-type 的
img[src]，再 object[data]；再唯一包装内部的 only-of-type img[src]，再 object[data]。
命中后按文档基址解析 URL，img 带 alt 时保留 {value,alt} 对象形态。

url：抑制条件与 photo 相同。依次尝试根 a[href]/area[href]；根的直接孩子中
only-of-type 的 a[href]，再 area[href]；再唯一包装内部的 only-of-type a[href]，
再 area[href]。命中后按文档基址解析 URL。

各项抑制仅看显式属性及嵌套根：新推导的 photo 不应再抑制 url，反之亦然。
root 自己的 p/u/e 类只在父根中使用，不算 root 的显式属性。
script/style/template 的内容不参与候选计数、显式属性统计或嵌套根统计；
这些元素自身仍属于元素孩子，不能借“忽略内容”改变 only-child/only-of-type。
