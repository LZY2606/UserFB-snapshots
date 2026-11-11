const assert = require('node:assert/strict');
const {Readability} = require('..');
const {JSDOM} = require('jsdom');
const JSDOMParser = require('../JSDOMParser');
const prose = 'A measured observation, with details about the sample, instruments and result. '.repeat(20);
const html = `<html><head><title>Survey report</title></head><body><article><p>${prose}</p><div class="negative"><table id="data"><caption>Observed values</caption><tbody><tr><td><a href="https://source.invalid/a">CELL_A</a></td><td>CELL_B</td></tr><tr><td>CELL_C</td><td>CELL_D</td></tr></tbody></table></div><table><tbody><tr><td id="single">SINGLE_CELL ${prose}</td></tr></tbody></table><p>${prose}</p></article></body></html>`;
for (const [name, factory] of [['jsdom', s => new JSDOM(s, {url:'https://source.invalid/'}).window.document], ['custom', s => new JSDOMParser().parse(s, 'https://source.invalid/')]]) {
  const result = new Readability(factory(html), {disableJSONLD:true}).parse();
  assert.ok(result && result.textContent.includes('CELL_A'));
  const doc = factory(result.content);
  assert.equal(doc.getElementById('data').getElementsByTagName('td').length, 4);
  assert.equal(doc.getElementById('single').tagName, 'P');
  assert.equal(result.length, result.textContent.length);
  console.log('BASE_TABLE_' + name.toUpperCase() + '_OK');
}
