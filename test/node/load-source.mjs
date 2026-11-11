import {registerHooks} from 'node:module';
registerHooks({resolve(specifier, context, nextResolve) {
  if (specifier === 'global/window') return nextResolve('global/window.js', context);
  if (specifier.startsWith('.') && !/\.[a-z]+$/i.test(specifier)) {
    return nextResolve(specifier + '.js', context);
  }
  return nextResolve(specifier, context);
}});
export const {default: Parser} = await import('../../src/parser.js');
