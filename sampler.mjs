export function sampleFields(root, paths) {
  return paths.map(path => {
    let value = root;
    for (let at = 0; at < path.length; at++) {
      if (value == null) return {status: 'missing', at};
      try { value = value[path[at]]; }
      catch (error) { return {status: 'error', at, error}; }
    }
    return {status: 'value', value};
  });
}
