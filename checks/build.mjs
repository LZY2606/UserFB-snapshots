import { build } from 'esbuild';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
export async function loadControls() {
  globalThis.DOMRect ??= class DOMRect { constructor(x=0,y=0,width=0,height=0) { Object.assign(this,{x,y,width,height}); } };
  const dir = await mkdtemp(join(tmpdir(), 'camera-controls-baseline-'));
  try {
    const outfile = join(dir, 'controls.mjs');
    await build({entryPoints:['src/index.ts'], bundle:true, format:'esm', platform:'node', outfile});
    return (await import(pathToFileURL(outfile).href)).default;
  } finally { await rm(dir, {recursive:true, force:true}); }
}
