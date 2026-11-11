import assert from 'node:assert/strict';
import http from 'node:http';
import {mkdtemp, rm, readFile, writeFile, readdir} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {pathToFileURL} from 'node:url';
import scrape from 'website-scraper';

const JS = 'application/javascript';
const HTML = 'text/html';

// routes: { '/path': {type, body} | {redirect, status} }
async function startServer (routes) {
	const hits = [];
	const server = http.createServer((req, res) => {
		hits.push(req.url);
		const route = routes[req.url];
		if (!route) {
			res.writeHead(404, {'content-type': 'text/plain'});
			res.end('not found');
			return;
		}
		if (route.redirect) {
			res.writeHead(route.status || 301, {location: route.redirect});
			res.end();
			return;
		}
		res.writeHead(200, {'content-type': route.type});
		res.end(route.body);
	});
	await new Promise((resolve, reject) => {
		server.once('error', reject);
		server.listen(0, '127.0.0.1', resolve);
	});
	return {server, hits, origin: `http://127.0.0.1:${server.address().port}`};
}

async function importSaved (directory, file) {
	return import(pathToFileURL(join(directory, file)).href);
}

// saved .js files are es modules, let node know that before importing them
async function allowModuleImport (directory) {
	await writeFile(join(directory, 'package.json'), '{"type":"module"}');
}

async function expectScrapeError (options) {
	let error = null;
	try {
		await scrape(options);
	} catch (e) {
		error = e;
	}
	assert.ok(error, 'scrape should be rejected');
	return error;
}

describe('Functional: modules', function () {
	let server, hits, origin, baseDir, directory;

	beforeEach(async function () {
		baseDir = await mkdtemp(join(tmpdir(), 'wsc-modules-'));
		directory = join(baseDir, 'saved');
	});

	afterEach(async function () {
		if (server) {
			await new Promise((resolve) => server.close(resolve));
			server = null;
		}
		await rm(baseDir, {recursive: true, force: true});
	});

	async function serve (routes) {
		({server, hits, origin} = await startServer(routes));
	}

	it('should archive the whole http graph of module dependencies and rewrite specifiers', async function () {
		const mainJs = [
			'import {value} from \'./dep.js\';',
			'export {extra} from \'./extra.js\';',
			'export const lazy = import(\'./lazy.js\');',
			'const name = \'dep2\';',
			'export const loadDynamic = () => import(name);',
			'// import fake1 from \'./fake1.js\';',
			'/* import fake2 from \'./fake2.js\'; */',
			'const s = "import fake3 from \'./fake3.js\'";',
			'const t = `import fake4 from \'./fake4.js\'`;',
			'const tpl = `${import(\'./tpl-dep.js\')}`;',
			'const r = /import fake5 from \'fake5.js\'/g;',
			'export const result = value;'
		].join('\n');
		const inlineJs = 'import {greet} from \'./inline-dep.js\'; export const g = greet;';

		await serve({
			'/index.html': {type: HTML, body: `<html><head>
				<script type="module" src="/main.js"></script>
				<script type="module">${inlineJs}</script>
			</head><body></body></html>`},
			'/main.js': {type: JS, body: mainJs},
			'/dep.js': {type: JS, body: 'export const value = 17;'},
			'/extra.js': {type: JS, body: 'export const extra = "ex";'},
			'/lazy.js': {type: JS, body: 'export default "lazy!";'},
			'/tpl-dep.js': {type: JS, body: 'export default "tpl!";'},
			'/inline-dep.js': {type: JS, body: 'export const greet = "hi";'}
		});

		await scrape({
			urls: [{url: `${origin}/index.html`, filename: 'index.html'}],
			directory,
			modules: true
		});

		// only real dependencies were requested, lookalike texts were not
		assert.deepEqual(hits.slice().sort(), [
			'/dep.js', '/extra.js', '/index.html', '/inline-dep.js', '/lazy.js', '/main.js', '/tpl-dep.js'
		]);

		const savedMain = await readFile(join(directory, 'js/main.js'), 'utf8');
		assert.match(savedMain, /from '\.\/dep\.js'/);
		assert.match(savedMain, /from '\.\/extra\.js'/);
		assert.match(savedMain, /import\('\.\/lazy\.js'\)/);
		assert.match(savedMain, /import\('\.\/tpl-dep\.js'\)/);
		assert.match(savedMain, /import\(name\)/); // runtime import() is left as is
		assert.match(savedMain, /fake1\.js/); // lookalikes stay untouched
		assert.doesNotMatch(savedMain, new RegExp(origin));

		const savedHtml = await readFile(join(directory, 'index.html'), 'utf8');
		assert.match(savedHtml, /src="js\/main\.js"/);
		assert.match(savedHtml, /from '\.\/js\/inline-dep\.js'/);

		await allowModuleImport(directory);
		const main = await importSaved(directory, 'js/main.js');
		assert.equal(main.result, 17);
		assert.equal(main.extra, 'ex');
		assert.equal((await main.lazy).default, 'lazy!');
	});

	it('should keep documents with different import maps isolated (document maps)', async function () {
		const page = (impl) => `<html><head>
			<script type="importmap">{"imports": {"app": "/${impl}.js"}}</script>
			<script type="module" src="/shared.js"></script>
		</head><body></body></html>`;

		await serve({
			'/page1.html': {type: HTML, body: page('impl-a')},
			'/page2.html': {type: HTML, body: page('impl-b')},
			'/shared.js': {type: JS, body: 'import {value} from "app"; export const result = value;'},
			'/impl-a.js': {type: JS, body: 'export const value = "A";'},
			'/impl-b.js': {type: JS, body: 'export const value = "B";'}
		});

		await scrape({
			urls: [
				{url: `${origin}/page1.html`, filename: 'page1.html'},
				{url: `${origin}/page2.html`, filename: 'page2.html'}
			],
			directory,
			modules: true
		});

		// raw bytes of /shared.js were downloaded once...
		assert.deepEqual(hits.filter((url) => url === '/shared.js'), ['/shared.js']);
		// ...but each document got its own rewritten copy
		assert.deepEqual((await readdir(join(directory, 'js'))).sort(), [
			'impl-a.js', 'impl-b.js', 'shared.js', 'shared_1.js'
		]);
		assert.match(await readFile(join(directory, 'page1.html'), 'utf8'), /src="js\/shared\.js"/);
		assert.match(await readFile(join(directory, 'page2.html'), 'utf8'), /src="js\/shared_1\.js"/);
		assert.match(await readFile(join(directory, 'js/shared.js'), 'utf8'), /from "\.\/impl-a\.js"/);
		assert.match(await readFile(join(directory, 'js/shared_1.js'), 'utf8'), /from "\.\/impl-b\.js"/);

		await allowModuleImport(directory);
		const first = await importSaved(directory, 'js/shared.js');
		const second = await importSaved(directory, 'js/shared_1.js');
		assert.equal(first.result, 'A');
		assert.equal(second.result, 'B');
	});

	it('should match import map scopes against post-redirect module urls (redirect scopes)', async function () {
		await serve({
			'/index.html': {type: HTML, body: `<html><head>
				<script type="importmap">{"imports": {"lib": "/lib-root.js"}, "scopes": {"/new/": {"lib": "/lib-new.js"}}}</script>
				<script type="module" src="/old/x.js"></script>
			</head><body></body></html>`},
			'/old/x.js': {redirect: '/new/x.js'},
			'/new/x.js': {type: JS, body: 'import {v} from "lib"; import {w} from "./sibling.js"; export const r = v + ":" + w;'},
			'/new/sibling.js': {type: JS, body: 'export const w = "sib";'},
			'/lib-new.js': {type: JS, body: 'export const v = "scoped";'},
			'/lib-root.js': {type: JS, body: 'export const v = "root";'}
		});

		await scrape({
			urls: [{url: `${origin}/index.html`, filename: 'index.html'}],
			directory,
			modules: true
		});

		// scope matched the post-redirect url /new/x.js, so the root mapping was never used
		assert.ok(hits.includes('/old/x.js'));
		assert.ok(hits.includes('/new/x.js'));
		assert.ok(!hits.includes('/lib-root.js'));

		// single local file for the redirected module
		assert.deepEqual((await readdir(join(directory, 'js'))).sort(), ['lib-new.js', 'sibling.js', 'x.js']);
		const savedX = await readFile(join(directory, 'js/x.js'), 'utf8');
		assert.match(savedX, /from "\.\/lib-new\.js"/);
		assert.match(savedX, /from "\.\/sibling\.js"/); // relative import resolved from the post-redirect url

		await allowModuleImport(directory);
		const x = await importSaved(directory, 'js/x.js');
		assert.equal(x.r, 'scoped:sib');
	});

	it('should share stable local files for diamond and circular dependencies (cycles)', async function () {
		await serve({
			'/index.html': {type: HTML, body: '<script type="module" src="/a.js"></script><script type="module" src="/x.js"></script>'},
			'/a.js': {type: JS, body: 'import {b} from "./b.js"; import {c} from "./c.js"; export const a = b + c;'},
			'/b.js': {type: JS, body: 'import {d} from "./d.js"; export const b = d + 1;'},
			'/c.js': {type: JS, body: 'import {d} from "./d.js"; export const c = d + 2;'},
			'/d.js': {type: JS, body: 'export const d = 10;'},
			'/x.js': {type: JS, body: 'import {yv} from "./y.js"; export const xv = "x"; export const fromY = yv;'},
			'/y.js': {type: JS, body: 'import {xv} from "./x.js"; export const yv = "y"; export function getX() { return xv; }'}
		});

		await scrape({
			urls: [{url: `${origin}/index.html`, filename: 'index.html'}],
			directory,
			modules: true
		});

		// diamond: d.js requested and saved once, cycle: x.js/y.js did not deadlock
		assert.deepEqual(hits.filter((url) => url === '/d.js'), ['/d.js']);
		assert.deepEqual((await readdir(join(directory, 'js'))).sort(), ['a.js', 'b.js', 'c.js', 'd.js', 'x.js', 'y.js']);
		assert.match(await readFile(join(directory, 'js/b.js'), 'utf8'), /from "\.\/d\.js"/);
		assert.match(await readFile(join(directory, 'js/c.js'), 'utf8'), /from "\.\/d\.js"/);

		await allowModuleImport(directory);
		const a = await importSaved(directory, 'js/a.js');
		assert.equal(a.a, 23);
		const x = await importSaved(directory, 'js/x.js');
		assert.equal(x.fromY, 'y');
		const y = await importSaved(directory, 'js/y.js');
		assert.equal(y.getX(), 'x');
	});

	it('should fail explicitly when an import map blocks a specifier (unresolved imports)', async function () {
		// null import map entry blocks the specifier
		await serve({
			'/index.html': {type: HTML, body: `<html><head>
				<script type="importmap">{"imports": {"blocked": null}}</script>
				<script type="module" src="/m.js"></script>
			</head><body></body></html>`},
			'/m.js': {type: JS, body: 'import {x} from "blocked"; export const y = x;'}
		});
		const error = await expectScrapeError({
			urls: [{url: `${origin}/index.html`, filename: 'index.html'}],
			directory,
			modules: true
		});
		assert.match(error.message, /blocked/);
		assert.match(error.message, /m\.js/);
	});

	it('should fail explicitly on unresolvable bare specifiers without requesting them (unresolved imports)', async function () {
		// unknown bare specifier is reported, not requested over http
		await serve({
			'/index.html': {type: HTML, body: '<script type="module" src="/m2.js"></script>'},
			'/m2.js': {type: JS, body: 'import {x} from "ghost-package"; export const y = x;'}
		});
		const error = await expectScrapeError({
			urls: [{url: `${origin}/index.html`, filename: 'index.html'}],
			directory,
			modules: true
		});
		assert.match(error.message, /ghost-package/);
		assert.match(error.message, /m2\.js/);
		assert.ok(!hits.some((url) => url.includes('ghost-package')));
	});

	it('should fail explicitly when urlFilter filters out a module (unresolved imports)', async function () {
		// module rejected by urlFilter breaks the dependency chain explicitly
		await serve({
			'/index.html': {type: HTML, body: '<script type="module" src="/main.js"></script>'},
			'/main.js': {type: JS, body: 'import {s} from "./secret.js"; export const v = s;'}
		});
		const error = await expectScrapeError({
			urls: [{url: `${origin}/index.html`, filename: 'index.html'}],
			directory,
			modules: true,
			urlFilter: (url) => !url.includes('secret.js')
		});
		assert.match(error.message, /secret\.js/);
		assert.match(error.message, /main\.js/);
	});

	it('should not follow module imports when modules option is disabled (legacy mode)', async function () {
		const mainJs = 'import {value} from "./dep.js"; export const result = value;';
		const inlineJs = 'import {z} from "./inline-dep.js";';

		await serve({
			'/index.html': {type: HTML, body: `<html><head>
				<script type="importmap">{"imports": {"app": "/impl.js"}}</script>
				<script type="module" src="/main.js"></script>
				<script type="module">${inlineJs}</script>
			</head><body></body></html>`},
			'/main.js': {type: JS, body: mainJs}
		});

		await scrape({
			urls: [{url: `${origin}/index.html`, filename: 'index.html'}],
			directory
		});

		// script src is saved as a plain resource, module imports are not followed
		assert.deepEqual(hits.slice().sort(), ['/index.html', '/main.js']);
		assert.equal(await readFile(join(directory, 'js/main.js'), 'utf8'), mainJs);
		const savedHtml = await readFile(join(directory, 'index.html'), 'utf8');
		assert.match(savedHtml, /src="js\/main\.js"/);
		assert.match(savedHtml, /import \{z\} from "\.\/inline-dep\.js";/);
		assert.match(savedHtml, /"app": "\/impl\.js"/);
	});
});
