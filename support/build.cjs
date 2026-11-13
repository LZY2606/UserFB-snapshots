const fs=require('node:fs');const path=require('node:path');
const ts=require('./vendor/typescript/typescript.js');
const root=path.join(__dirname,'..');
fs.mkdirSync(path.join(root,'dist'),{recursive:true});
for(const name of fs.readdirSync(path.join(root,'src')).filter(x=>x.endsWith('.ts')&&!x.endsWith('.d.ts'))) {
 const file=path.join(root,'src',name);
 const out=ts.transpileModule(fs.readFileSync(file,'utf8'),{fileName:file,reportDiagnostics:true,compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.CommonJS,esModuleInterop:true}});
 const errors=(out.diagnostics||[]).filter(x=>x.category===ts.DiagnosticCategory.Error);
 if(errors.length){for(const e of errors)console.error(ts.flattenDiagnosticMessageText(e.messageText,'\n'));process.exit(1);}
 fs.writeFileSync(path.join(root,'dist',name.replace(/\.ts$/,'.js')),out.outputText);
}
console.log('Built production src/*.ts using bundled TypeScript '+ts.version+' (transpile, no typecheck)');
