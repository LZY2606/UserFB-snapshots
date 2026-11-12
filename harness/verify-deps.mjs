import {readFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
const files=JSON.parse(readFileSync(new URL('./dependency-files.json',import.meta.url)));
for(const [path,hash] of Object.entries(files)) {
 const actual=createHash('sha256').update(readFileSync(new URL(path,import.meta.url))).digest('hex');
 if(actual!==hash) throw Error(`dependency differs: ${path}`);
}
console.log(`DEPENDENCIES_OK files=${Object.keys(files).length}`);
