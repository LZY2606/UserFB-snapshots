import assert from 'node:assert/strict';
export function check(t,actual,expected){
 assert.deepStrictEqual(actual,expected);
 console.log('CHECK_CONTENT_CHECK:'+t.name);
}
