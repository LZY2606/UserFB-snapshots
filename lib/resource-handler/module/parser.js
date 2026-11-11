import {init, parse} from 'es-module-lexer';

const lexerReady = init;

/**
 * Parses javascript module source and returns its static imports,
 * export ... from statements and dynamic imports with string literal arguments.
 * Dynamic imports with non-literal arguments and import.meta are not included,
 * so lookalike text in comments, strings, regexps and template literals is never reported.
 * @param {string} source - javascript module source code
 * @returns {Promise<Array<{specifier: string, start: number, end: number, hasAttributes: boolean}>>}
 */
async function parseModuleImports (source) {
	await lexerReady;
	const [imports] = parse(source);
	return imports
		.filter((record) => record.type !== 'import-meta' && typeof record.specifier === 'string')
		.map((record) => ({
			specifier: record.specifier,
			// static imports point at the specifier itself, dynamic imports include the quotes
			start: record.type === 'dynamic' ? record.start + 1 : record.start,
			end: record.type === 'dynamic' ? record.end - 1 : record.end,
			hasAttributes: record.attributesStart !== -1
		}));
}

export {parseModuleImports};
