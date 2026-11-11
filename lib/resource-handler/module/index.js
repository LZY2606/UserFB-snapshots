import logger from '../../logger.js';
import Resource from '../../resource.js';
import types from '../../config/resource-types.js';
import {getUrl, normalizeUrl} from '../../utils/index.js';
import {parseImportMap, emptyImportMap, resolveSpecifier} from './import-map.js';
import {parseModuleImports} from './parser.js';

const HTTP_URL_REGEXP = /^https?:\/\//i;

const JAVASCRIPT_MIME_TYPES = [
	'text/javascript',
	'application/javascript',
	'text/ecmascript',
	'application/ecmascript',
	'application/x-javascript',
	'text/x-javascript'
];

class ModuleError extends Error {
	constructor (message) {
		super(message);
		this.name = 'ModuleError';
	}
}

function isModuleScript ($element) {
	return $element.is('script') && ($element.attr('type') || '').trim().toLowerCase() === 'module';
}

function toUtf8Text (text, encoding) {
	return encoding === 'utf8' ? text : Buffer.from(text, 'binary').toString('utf8');
}

function applyReplacements (text, replacements) {
	let updatedText = text;
	const sortedReplacements = replacements.slice().sort((a, b) => b.start - a.start);
	for (const replacement of sortedReplacements) {
		updatedText = updatedText.slice(0, replacement.start) + replacement.reference + updatedText.slice(replacement.end);
	}
	return updatedText;
}

// A relative path between archive files is only a valid module specifier
// when it starts with ./ or ../
function toModuleSpecifier (reference) {
	if (reference.startsWith('.') || reference.startsWith('/') || HTTP_URL_REGEXP.test(reference)) {
		return reference;
	}
	return './' + reference;
}

/**
 * Archives the javascript module graph of html documents.
 * Every document gets its own resolution context (its inline import map), so the same
 * module url imported from different documents is saved as separate files with their own
 * rewritten text, while inside one document diamonds and cycles share a single local file.
 * Downloaded module bytes are shared between contexts (see Scraper.requestModule).
 */
class ModuleHandler {
	constructor ({requestModule, generateFilename, getReference, saveResource}) {
		this.requestModule = requestModule;
		this.generateFilename = generateFilename;
		this.getReference = getReference;
		this.saveResource = saveResource;
	}

	async processDocument ($, documentResource) {
		const documentUrl = documentResource.getUrl();
		const context = {
			documentUrl,
			importMap: emptyImportMap(),
			nodePromises: new Map(), // normalized module url -> Promise<node>
			processed: [] // promises of node processing (rewrite + save)
		};

		const moduleScripts = [];
		let importMapParsed = false;
		$('script').each((i, element) => {
			const $element = $(element);
			const type = ($element.attr('type') || '').trim().toLowerCase();
			if (type === 'importmap') {
				if (importMapParsed) {
					return;
				}
				if (moduleScripts.length > 0) {
					throw new ModuleError(`Import map in ${documentUrl} should be placed before any module script`);
				}
				context.importMap = parseImportMap($element.text(), documentUrl);
				importMapParsed = true;
			} else if (type === 'module') {
				moduleScripts.push($element);
			}
		});

		for (const $element of moduleScripts) {
			const src = $element.attr('src');
			if (src) {
				await this.processExternalEntry($element, src, context, documentResource);
			} else {
				await this.processInlineEntry($element, context, documentResource);
			}
		}

		await this.drainContext(context);
	}

	async processExternalEntry ($element, src, context, documentResource) {
		const url = getUrl(context.documentUrl, src);
		if (!HTTP_URL_REGEXP.test(url)) {
			logger.debug(`skipping non-http(s) module script ${src} in ${context.documentUrl}`);
			return;
		}
		const node = await this.ensureNode(context, url, documentResource);
		const {reference} = await this.getReference({
			parentResource: documentResource,
			resource: node.resource,
			originalReference: src
		});
		if (reference) {
			$element.attr('src', reference);
		}
	}

	async processInlineEntry ($element, context, documentResource) {
		const text = $element.text();
		if (!text.trim()) {
			return;
		}
		// inline modules belong to the document: its url is the referrer for their imports
		const updatedText = await this.rewriteModuleText(text, context.documentUrl, context, documentResource);
		$element.text(updatedText);
	}

	async rewriteModuleText (text, referrerUrl, context, parentResource) {
		let imports;
		try {
			imports = await parseModuleImports(text);
		} catch (error) {
			throw new ModuleError(`Failed to parse module ${referrerUrl}: ${error.message}`);
		}

		const replacements = [];
		for (const moduleImport of imports) {
			if (moduleImport.hasAttributes) {
				throw new ModuleError(`Import attributes (json/css modules) are not supported: "${moduleImport.specifier}" imported from ${referrerUrl}`);
			}
			const resolvedUrl = resolveSpecifier(moduleImport.specifier, referrerUrl, context.importMap);
			if (!HTTP_URL_REGEXP.test(resolvedUrl)) {
				logger.debug(`skipping non-http(s) module ${resolvedUrl} imported from ${referrerUrl}`);
				continue;
			}
			const dependencyNode = await this.ensureNode(context, resolvedUrl, parentResource);
			const {reference} = await this.getReference({
				parentResource,
				resource: dependencyNode.resource,
				originalReference: moduleImport.specifier
			});
			if (reference) {
				replacements.push({
					start: moduleImport.start,
					end: moduleImport.end,
					reference: toModuleSpecifier(reference)
				});
			}
		}
		return applyReplacements(text, replacements);
	}

	ensureNode (context, url, parentResource) {
		const requestKey = normalizeUrl(url);
		if (!context.nodePromises.has(requestKey)) {
			context.nodePromises.set(requestKey, this.createNode(context, url, parentResource));
		}
		return context.nodePromises.get(requestKey);
	}

	async createNode (context, url, parentResource) {
		const data = await this.requestModule({url, parentResource});
		if (!data) {
			throw new ModuleError(`Can not archive module ${url} imported from ${parentResource.getUrl()}: request was filtered out or failed, module dependency chain is broken`);
		}

		// after redirects the final url is the node identity, so the same target
		// shares one local file and is used as referrer for its own imports
		const finalKey = normalizeUrl(data.url);
		if (finalKey !== normalizeUrl(url) && context.nodePromises.has(finalKey)) {
			return context.nodePromises.get(finalKey);
		}

		if (data.mimeType && !JAVASCRIPT_MIME_TYPES.includes(data.mimeType)) {
			throw new ModuleError(`Can not archive module ${data.url} imported from ${parentResource.getUrl()}: expected javascript but got content-type "${data.mimeType}"`);
		}

		const resource = new Resource(data.url);
		resource.setType(types.js);
		resource.parent = parentResource;

		const node = {
			url: data.url,
			text: toUtf8Text(data.text, data.encoding),
			resource,
			processed: null
		};
		context.nodePromises.set(finalKey, Promise.resolve(node));

		// filename is assigned before dependencies are processed, so circular
		// imports resolve each other's local target without waiting forever
		const filename = await this.generateFilename(resource, {url: data.url, mimeType: data.mimeType});
		resource.setFilename(filename);

		node.processed = this.processNode(node, context);
		node.processed.catch(() => {}); // errors are reported via drainContext
		context.processed.push(node.processed);

		return node;
	}

	async processNode (node, context) {
		const updatedText = await this.rewriteModuleText(node.text, node.url, context, node.resource);
		node.resource.setText(updatedText);
		node.resource.setEncoding('utf8');
		await this.saveResource(node.resource);
	}

	async drainContext (context) {
		let index = 0;
		while (index < context.processed.length) {
			const batch = context.processed.slice(index);
			index = context.processed.length;
			await Promise.all(batch);
		}
	}
}

export default ModuleHandler;
export {isModuleScript};
