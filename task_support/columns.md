列坐标从 1 开始。这里只处理 HTML 命名空间的 table、colgroup、col、thead/tbody/tfoot、tr、td/th；golang.org/x/net/html 中 HTML namespace 是空字符串。DOM 是唯一输入，不读 CSS。
- 表格的行按直属行组的 DOM 次序排列；连续的直属 tr 作为一个隐式行组。只读各行的直属单元格，不进入嵌套 table。
- 单元格从当前行游标向右找第一个未被前行 rowspan 占用的位置，覆盖 colspan × rowspan 矩形，再把游标移到矩形之后；矩形内任何其他占位造成冲突，则本表的三种新列条件均不匹配任何单元格。rowspan=0 覆盖本行组剩余行，正值不能超过当前行组；新行组清空旧占位。
- span/colspan/rowspan 只接受去除两端 ASCII 空白后的十进制非负整数；缺失、非数字或整数溢出按 1。span/colspan 的 0 按 1、正值封顶 1000；rowspan 正值封顶 65534。
- 按 table 直属 col/colgroup 的 DOM 次序建立声明列范围。有直属 col 孩子的 colgroup 忽略自身 span，子 col 的范围依次分配，group 覆盖子 col 的并集；无子 col 的 group 由自身 span 分配。其他子孙不参加列声明。
- 全表宽度为所有单元格右端和声明列右端的最大值。未被单元格占满的声明列仍计入总宽。没有行或单元格的表不产生 cell 匹配。各表独立。
- || 的右端必须是本表 td/th，左端从同表内覆盖该单元格任一逻辑列的真实 col/colgroup 节点中寻找满足完整左选择器者；不创建隐式 col 节点。两个位置伪类只匹配 td/th，正向看其任一逻辑列，逆向用全表宽度+1-列号，再套现有 An+B 的非负 n 条件。
- 这里把 2022 Selectors 4 网格选择语法用于离线 HTML 采集，不声明实现浏览器布局。正常 QueryAll/Filter/Match、String、Specificity 都走原生入口，不新增旁路查询器。
