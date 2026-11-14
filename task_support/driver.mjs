import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';
import { execute } from './check.mjs';
const [target,group,token]=process.argv.slice(2);
await import(pathToFileURL(resolve(target)).href);
const checks=await execute(group,['lifecycle','geometry','query','work']);
console.log(`GROUP_COMPLETE ${group} checks=${checks} token=${token}`);
