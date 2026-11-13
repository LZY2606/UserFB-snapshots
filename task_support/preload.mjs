import { registerHooks } from 'node:module';
registerHooks({resolve(specifier, context, nextResolve) {
 const entries={'path-browserify':'./vendor/path-browserify/index.js','ua-parser-js':'./vendor/ua-parser-js/src/ua-parser.js'};
 if(Object.hasOwn(entries,specifier))return {url:new URL(entries[specifier],import.meta.url).href,shortCircuit:true};
 return nextResolve(specifier,context);
}});
globalThis.window = {console,performance:{addEventListener(){},removeEventListener(){}}};
