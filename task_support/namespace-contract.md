# 混合页面字段接口与范围

在 html.hpp 为 html::node 增加 const 成员：std::string namespace_uri() const、bool has_attr_ns(const std::string& uri, const std::string& local) const、std::string get_attr_ns(const std::string& uri, const std::string& local) const。不存在的属性返回空字符串，has_attr_ns 区分缺失和空值。元素使用 HTML、SVG、MathML 的标准 URI，非元素返回空URI；普通属性URI为空，外来属性按标准调整后的 local name 与 namespace 识别。保留 get_attr/has_attr 的限定名字接口，例如 get_attr("xlink:href")、get_attr("viewBox")。utils::make_node 创建的普通 tag 默认属于 HTML。

增加 html::selector(const std::string&, const std::map<std::string,std::string>& bindings)，绑定是 prefix→URI 的编译期快照，空prefix表示默认元素namespace。旧构造器相当于无绑定。

支持 type 的 p|name、p|*、*|name、|name，以及属性的 [p|name]、[*|name]、[|name] 和已有所有属性比较运算符。例：[x|href|='urn'] 里的第二个 | 是 |= 运算符。无prefix元素名在有默认绑定时只匹配默认URI，无默认绑定时不限URI；无prefix属性名只匹配无namespace属性，默认绑定不影响它。默认元素namespace也约束省略类型的复合选择器。空prefix显式限定无namespace，*匹配任意URI。HTML元素名字按ASCII不区分大小写，外来元素及其属性local name按调整后的大小写精确比较。id/class原有规则保留。

未声明prefix或不完整限定名使整个 selector 无效，operator bool 返回 false；无效的有参selector注册callback不得变成全节点callback。原来的无参selector/unfiltered回调仍按旧行为。namespace条件可以组合现有id/class/属性、并集和查询中的父子/祖先关系；回调一致性针对已有支持的单个复合选择器，不扩展其关系回调能力。编译后改bindings不能改变选择器。

解析按HTML语法中foreign content规则维护SVG/MathML身份，包括SVG标签与属性大小写调整、MathML definitionURL、xlink/xml/xmlns外来属性调整；HTML积分点、MathML文本积分点、annotation-xml encoding与svg例外、foreign breakout、外来结束标签的栈回退。CDATA只在当前调整节点为非HTML时认作文本；回到HTML仍按原bogus comment规则。HTML积分点本身仍是外来节点，只有其相应子token回到HTML。rawtext与void规则只在HTML namespace生效；外来self-closing正确退栈。HTML fallback沿用本库既有树规则，不要求补齐HTML5表格/收养算法、字符实体或片段解析。

string和istream入口应一致；同一parser复用不残留namespace状态。开标签回调前，namespace、canonical name和属性身份已经确定。DOM复制构造、移动构造与append保留子树身份和有效parent指针。原set_attr（两重载）/del_attr在外来节点上按规范化名字增删时，同步namespace属性查询；不实现任意前缀声明或namespace setter。HTML中的xmlns属性不把元素改成XML解析模式。

to_html/to_raw_html使用节点自身namespace决定名字、空元素和文本处理；把含CDATA的foreign子树输出后，本库再次解析必须得到相同的元素/属性namespace、非排版文本与关系。允许使用合法CDATA写法，CDATA内原本有意义的空白不能被折叠。pretty输出额外的排版纯空白不参与往返对比。普通HTML的旧输出保持兼容。任意非法XML内容、字节级无损还原、浏览器渲染和网络抓取不在范围内。

规则出处供查阅：HTML Standard 的 parsing foreign content、tree construction dispatcher、adjust SVG/MathML/foreign attributes；Selectors Level 3 的 type/universal/attribute namespaces。
https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inforeign
https://www.w3.org/TR/selectors-3/#typenmsp
https://www.w3.org/TR/selectors-3/#attrnmsp
