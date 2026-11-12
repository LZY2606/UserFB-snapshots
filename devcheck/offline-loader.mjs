import { registerHooks } from 'node:module';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
registerHooks({resolve(specifier,context,nextResolve){
 if(specifier.startsWith('.') && specifier.endsWith('.js') && context.parentURL){
  const target=new URL(specifier.slice(0,-3)+'.ts',context.parentURL);
  if(target.protocol==='file:' && existsSync(fileURLToPath(target)))return nextResolve(target.href,context);
 }
 return nextResolve(specifier,context);
}});
