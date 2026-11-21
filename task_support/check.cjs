const test = require('node:test');
const assert = require('node:assert/strict');
exports.checked = (name, body) => test(name, async t => {
  let checks=0, returned=false;
  t.after(()=> {
    assert.ok(returned,'test body must return normally');
    assert.ok(checks>0,'test group must execute content assertions');
    if(returned&&checks) console.log(`TASK_GROUP_DONE ${name} checks=${checks}`);
  });
  await body({check:(actual,expected)=>{checks++;assert.deepStrictEqual(actual,expected);},t});
  returned=true;
});
