import {isPlainObject} from '../../utils/index.js';

class ImportMapError extends Error {
	constructor (message) {
		super(message);
		this.name = 'ImportMapError';
	}
}

function parseAbsoluteUrl (specifier) {
	try {
		return new URL(specifier).href;
	} catch {
		return null;
	}
}

function isUrlLike (specifier) {
	return specifier.startsWith('/') || specifier.startsWith('./') || specifier.startsWith('../');
}

// Resolves a URL-like specifier (absolute url, or starting with /, ./, ../) against baseUrl.
// Returns absolute url string or null for bare specifiers and invalid urls.
function resolveUrlLike (specifier, baseUrl) {
	const absoluteUrl = parseAbsoluteUrl(specifier);
	if (absoluteUrl) {
		return absoluteUrl;
	}
	if (isUrlLike(specifier)) {
		try {
			return new URL(specifier, baseUrl).href;
		} catch {
			return null;
		}
	}
	return null;
}

function normalizeSpecifierMap (map, baseUrl, importMapUrl) {
	if (!isPlainObject(map)) {
		throw new ImportMapError(`Invalid import map at ${importMapUrl}: expected an object of specifier mappings`);
	}
	const normalized = new Map();
	for (const [key, rawValue] of Object.entries(map)) {
		const normalizedKey = resolveUrlLike(key, baseUrl) || key;
		if (rawValue === null) {
			normalized.set(normalizedKey, null);
			continue;
		}
		if (typeof rawValue !== 'string') {
			throw new ImportMapError(`Invalid import map at ${importMapUrl}: address for "${key}" should be a string or null`);
		}
		const value = resolveUrlLike(rawValue, baseUrl);
		if (value === null) {
			// Matches browser behavior: invalid addresses are dropped, so such specifiers
			// fail to resolve instead of being requested as-is
			continue;
		}
		if (normalizedKey.endsWith('/') && !value.endsWith('/')) {
			throw new ImportMapError(`Invalid import map at ${importMapUrl}: address "${rawValue}" for prefix key "${key}" should end with "/"`);
		}
		normalized.set(normalizedKey, value);
	}
	return normalized;
}

function parseImportMap (jsonText, baseUrl) {
	let parsed;
	try {
		parsed = JSON.parse(jsonText);
	} catch (error) {
		throw new ImportMapError(`Failed to parse import map at ${baseUrl}: ${error.message}`);
	}
	if (!isPlainObject(parsed)) {
		throw new ImportMapError(`Invalid import map at ${baseUrl}: expected a json object`);
	}
	const rawScopes = parsed.scopes || {};
	if (!isPlainObject(rawScopes)) {
		throw new ImportMapError(`Invalid import map at ${baseUrl}: "scopes" should be an object`);
	}
	const scopes = new Map();
	for (const [scopeKey, scopeMap] of Object.entries(rawScopes)) {
		const normalizedScopeKey = resolveUrlLike(scopeKey, baseUrl);
		if (normalizedScopeKey === null) {
			throw new ImportMapError(`Invalid import map at ${baseUrl}: scope "${scopeKey}" can not be resolved to a url`);
		}
		scopes.set(normalizedScopeKey, normalizeSpecifierMap(scopeMap, baseUrl, baseUrl));
	}
	return {
		imports: normalizeSpecifierMap(parsed.imports || {}, baseUrl, baseUrl),
		scopes
	};
}

function emptyImportMap () {
	return {imports: new Map(), scopes: new Map()};
}

// Returns matched address, null for a null (blocking) entry or undefined when nothing matched
function matchPrefix (specifier, map) {
	let matchedKey = null;
	let matchedValue;
	for (const [key, value] of map) {
		if (key.endsWith('/') && specifier.startsWith(key) && (matchedKey === null || key.length > matchedKey.length)) {
			matchedKey = key;
			matchedValue = value;
		}
	}
	if (matchedKey === null) {
		return undefined;
	}
	if (matchedValue === null) {
		return null;
	}
	return matchedValue + specifier.slice(matchedKey.length);
}

// Returns matched address, null for a null (blocking) entry or undefined when nothing matched
function resolveInMap (specifier, asUrl, map) {
	const key = asUrl === null ? specifier : asUrl;
	if (map.has(key)) {
		return map.get(key);
	}
	return matchPrefix(key, map);
}

function resolveMatch (match, specifier, referrerUrl) {
	if (match === null) {
		throw new ImportMapError(`Specifier "${specifier}" imported from ${referrerUrl} was blocked by a null entry in the import map`);
	}
	return match;
}

// Resolves a module specifier the way a browser does:
// https://html.spec.whatwg.org/multipage/webappapis.html#resolve-a-module-specifier
// Longest matching scope wins, inside a scope/imports longest prefix key wins.
function resolveSpecifier (specifier, referrerUrl, importMap) {
	const asUrl = resolveUrlLike(specifier, referrerUrl);

	const matchingScopes = Array.from(importMap.scopes.keys())
		.filter((scopePrefix) => referrerUrl.startsWith(scopePrefix))
		.sort((a, b) => b.length - a.length);

	for (const scopePrefix of matchingScopes) {
		const match = resolveInMap(specifier, asUrl, importMap.scopes.get(scopePrefix));
		if (match !== undefined) {
			return resolveMatch(match, specifier, referrerUrl);
		}
	}

	const match = resolveInMap(specifier, asUrl, importMap.imports);
	if (match !== undefined) {
		return resolveMatch(match, specifier, referrerUrl);
	}

	if (asUrl !== null) {
		return asUrl;
	}

	throw new ImportMapError(`Unable to resolve bare specifier "${specifier}" imported from ${referrerUrl}: no matching import map entry`);
}

export {ImportMapError, parseImportMap, emptyImportMap, resolveSpecifier};
