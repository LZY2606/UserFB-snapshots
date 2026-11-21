const fs = require('node:fs');
const path = require('node:path');
const ts = require('../vendor/typescript/lib/typescript.js');
const root = path.resolve(__dirname, '..');
const out = path.join(root, '.task-build');
fs.rmSync(out, {recursive:true, force:true});
fs.mkdirSync(out);
fs.writeFileSync(path.join(out,'package.json'), '{"type":"commonjs"}\n');
function walk(dir) {
  for (const e of fs.readdirSync(dir,{withFileTypes:true})) {
    const p = path.join(dir,e.name);
    if (e.isDirectory()) walk(p);
    else if (p.endsWith('.ts') && !p.endsWith('.d.ts') && !p.endsWith('.test.ts')) {
      const rel = path.relative(path.join(root,'src'),p).replace(/\.ts$/,'.js');
      const result = ts.transpileModule(fs.readFileSync(p,'utf8'), {
        fileName:p, reportDiagnostics:true,
        compilerOptions:{target:ts.ScriptTarget.ES2022, module:ts.ModuleKind.CommonJS,
          esModuleInterop:true, sourceMap:false}
      });
      const errors=(result.diagnostics||[]).filter(d=>d.category===ts.DiagnosticCategory.Error);
      if(errors.length) throw Error(ts.formatDiagnosticsWithColorAndContext(errors,{
        getCurrentDirectory:()=>root,getCanonicalFileName:x=>x,getNewLine:()=> '\n'}));
      const dest=path.join(out,rel);fs.mkdirSync(path.dirname(dest),{recursive:true});
      fs.writeFileSync(dest,result.outputText);
    }
  }
}
walk(path.join(root,'src'));
