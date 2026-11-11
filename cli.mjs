import {readFileSync} from 'node:fs';
import {nearestCube} from './cube.mjs';
const mode=process.argv[2]??'nearest';
const cube=JSON.parse(readFileSync(new URL('./fixtures/axis.json',import.meta.url),'utf8'));
if(mode!=='nearest')throw new Error(`unsupported mode: ${mode}`);
console.log(JSON.stringify(nearestCube(cube,[1,0,0])));
