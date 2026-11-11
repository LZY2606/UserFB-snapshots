import {registerHooks} from 'node:module';
import {readFileSync} from 'node:fs';
registerHooks({resolve(specifier, context, nextResolve) {
  if ((specifier.startsWith('.') || specifier.startsWith('@videojs/vhs-utils/es/') || specifier === 'global/window') && !/\.[a-z]+$/i.test(specifier)) {
    return nextResolve(specifier + '.js', context);
  }
  return nextResolve(specifier, context);
}, load(url, context, nextLoad) {
  if (url.endsWith('/package.json')) {
    const {version} = JSON.parse(readFileSync(new URL(url), 'utf8'));
    return {format: 'module', shortCircuit: true, source: `export const version = ${JSON.stringify(version)};`};
  }
  return nextLoad(url, context);
}});
export const {parse} = await import('../../src/index.js');
