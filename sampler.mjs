/**
 * 按字段路径采样运行时对象。
 *
 * sampleFields(root, paths) — 同步函数。
 *
 * 参数：
 *   root  非 null 的对象或函数，否则抛 TypeError。
 *   paths 路径数组，按给定顺序处理。每条路径是至少一段的字符串数组；
 *         段是精确的属性名（可以是空串），不按点号拆分、不做数值转换。
 *         整个 paths 在触碰任何页面属性之前完整校验，无效抛 TypeError。
 *         root、paths 及取到的描述符都不会被修改。
 *
 * 观察语义：
 *   一次调用之内，同一个对象的同一个自有属性只观察一次——数据属性、
 *   getter 返回值、getter 异常、属性不存在都按「对象身份 + 属性键」缓存，
 *   与路径写法、getter 函数身份无关；下一次调用重新观察。属性在第一次
 *   真正被路径用到时才读取，不提前枚举其他字段。只读自有描述符，不回退
 *   到原型；getter 的 this 是持有该自有属性的对象；只有 setter 的自有
 *   属性视为存在、值为 undefined。
 *
 * 返回值：每条路径一个结果，为以下三种之一——
 *   {status:'value', value}    走完全部路径段；末段自有值为 undefined 也算 value
 *   {status:'missing', at}     at 是第一个走不下去的段下标：自有属性缺失，
 *                              或当前值是 null/原始值却还有下一段要走
 *   {status:'error', at, error} getter 抛出的值保持原身份放在 error；缓存的
 *                              异常沿不同别名路径重用时 at 按各自路径计算
 *   结果原样返回，不 await、不序列化、不克隆；终点是 thenable 也原样返回。
 */

function isObjectLike(value) {
  return (typeof value === 'object' && value !== null) || typeof value === 'function';
}

export function sampleFields(root, paths) {
  if (!isObjectLike(root)) {
    throw new TypeError('sampleFields: root must be a non-null object or function');
  }
  if (!Array.isArray(paths)) {
    throw new TypeError('sampleFields: paths must be an array of paths');
  }
  for (const path of paths) {
    if (!Array.isArray(path) || path.length === 0) {
      throw new TypeError('sampleFields: each path must be a non-empty array of property names');
    }
    for (const segment of path) {
      if (typeof segment !== 'string') {
        throw new TypeError('sampleFields: each path segment must be a string');
      }
    }
  }

  // 本次调用的观察缓存：对象身份 -> 属性键 -> 观察结果。
  const cache = new WeakMap();

  function observe(object, key) {
    let perObject = cache.get(object);
    if (perObject === undefined) {
      perObject = new Map();
      cache.set(object, perObject);
    } else if (perObject.has(key)) {
      return perObject.get(key);
    }
    let observation;
    const descriptor = Object.getOwnPropertyDescriptor(object, key);
    if (descriptor === undefined) {
      observation = {kind: 'missing'};
    } else if ('value' in descriptor) {
      observation = {kind: 'value', value: descriptor.value};
    } else if (typeof descriptor.get === 'function') {
      try {
        observation = {kind: 'value', value: descriptor.get.call(object)};
      } catch (error) {
        observation = {kind: 'error', error};
      }
    } else {
      // 自有访问器只有 setter：视为存在，值为 undefined。
      observation = {kind: 'value', value: undefined};
    }
    perObject.set(key, observation);
    return observation;
  }

  return paths.map(path => {
    let current = root;
    for (let at = 0; at < path.length; at++) {
      if (!isObjectLike(current)) {
        return {status: 'missing', at};
      }
      const observation = observe(current, path[at]);
      if (observation.kind === 'missing') {
        return {status: 'missing', at};
      }
      if (observation.kind === 'error') {
        return {status: 'error', at, error: observation.error};
      }
      current = observation.value;
    }
    return {status: 'value', value: current};
  });
}
