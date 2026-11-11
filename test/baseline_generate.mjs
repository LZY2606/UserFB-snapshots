import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
const require = createRequire(new URL('./baseline-tools/package.json', import.meta.url));
const { preprocess } = require('preprocess');
function walk(dir) {
  for (const e of fs.readdirSync(dir, { withFileTypes:true })) {
    const file = path.join(dir,e.name);
    if (e.isDirectory()) walk(file);
    else if (e.name.endsWith('.template.js')) {
      for (const indirect of [false,true]) {
        const out = file.replace(/\.template\.js$/,indirect?'_indirect.generated.js':'.generated.js');
        const code = preprocess(fs.readFileSync(file,'utf8'), {INDIRECT:indirect,INDIRECT_STRING:indirect?'_indirect':''}, {type:'js'});
        fs.writeFileSync(out,code);
      }
    }
  }
}
walk('src');
console.log('BVH direct/indirect templates generated');
