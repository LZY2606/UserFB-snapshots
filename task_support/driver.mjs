import {resolve} from 'node:path';
import {pathToFileURL} from 'node:url';
import {run} from './check.mjs';
await import(pathToFileURL(resolve(process.argv[2])).href);
await run();
