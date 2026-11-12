import { registerHooks, createRequire } from 'node:module';
import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
const src = new URL('../src/', import.meta.url).href;
const requireVendor = createRequire(new URL('./vendor/entry.cjs', import.meta.url));
const readableStream = requireVendor.resolve('readable-stream');
registerHooks({
 resolve(specifier, context, nextResolve) {
  if (specifier === 'readable-stream') return nextResolve(readableStream, context);
  if (context.parentURL?.startsWith(src) && specifier.startsWith('.')) {
   const url = new URL(specifier, context.parentURL);
   if (!url.pathname.endsWith('.js') && existsSync(fileURLToPath(url)+'.js')) return { url:url.href+'.js', shortCircuit:true };
  }
  return nextResolve(specifier, context);
 },
 load(url, context, nextLoad) {
  if (url.startsWith(src) && url.endsWith('.js')) return {format:'module',source:readFileSync(fileURLToPath(url),'utf8'),shortCircuit:true};
  return nextLoad(url, context);
 }
});
