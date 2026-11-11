页面字段采样的离线最小起点。Node >=22.18，无安装依赖。
`node baseline.mjs` 演示修复后的单次观察语义；`node --test test/*.test.mjs` 验证目标契约。
输入是已经取得的普通 JS 对象及按属性名分段的路径，不包含浏览器连接、网络或任何真实账户数据。

## API：`sampleFields(root, paths)`（同步）

- `root`：非 null 的对象或函数，否则抛 `TypeError`。
- `paths`：路径数组，按给定顺序处理。每条路径是至少一段的字符串数组；段是精确的属性名
  （可以是空串），不按点号拆分、不做数值转换。整个 `paths` 在触碰任何页面属性之前完整校验，
  无效抛 `TypeError`。`root`、`paths` 及取到的描述符都不会被修改。
- 观察语义：一次调用之内，同一个对象的同一个自有属性只观察一次——数据属性、getter 返回值、
  getter 异常、属性不存在都按「对象身份 + 属性键」缓存，与路径写法、getter 函数身份无关；
  下一次调用重新观察。属性在第一次真正被路径用到时才读取，不提前枚举其他字段。只读自有描述符，
  不回退到原型；getter 的 `this` 是持有该自有属性的对象；只有 setter 的自有属性视为存在、
  值为 `undefined`。
- 返回：每条路径一个结果，为 `{status:'value', value}`、`{status:'missing', at}`、
  `{status:'error', at, error}` 之一。`at` 是第一个走不下去的段下标；getter 抛出的值保持
  原身份放进 `error`，缓存的异常沿不同别名路径重用时 `at` 按各自路径计算。结果原样返回，
  不 await、不序列化、不克隆；终点是 thenable 也原样返回。
