const groups = new Map();
let current = null;
export function runGroup(name, body) {
  if(typeof name !== 'string' || typeof body !== 'function' || groups.has(name) || body.skip) throw Error('invalid or duplicate group');
  groups.set(name,body);
}
export function check(value, message='content check') {
  if(!current) throw Error('assertion outside active group');
  current.checks++;
  if(!value) {current.failed=true;throw Error(message);}
}
export async function execute(name, expected) {
  if(groups.size !== expected.length || expected.some(g=>!groups.has(g))) throw Error('missing or unknown group');
  current={checks:0,failed:false};
  await groups.get(name)();
  if(!current.checks || current.failed) throw Error('no assertions or a swallowed assertion failure');
  const n=current.checks;current=null;return n;
}
