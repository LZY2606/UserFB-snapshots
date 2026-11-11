'use strict';
const fs=require('node:fs'),path=require('node:path'),os=require('node:os');
const esbuild=require('./tools/node_modules/esbuild');
exports.withBundle=async function(entry,run){
 const b=fs.mkdtempSync(path.join(os.tmpdir(),'composition-'));
 try{
  fs.symlinkSync(path.join(__dirname,'tools/node_modules'),path.join(b,'node_modules'),'dir');
  const outfile=path.join(b,'entry.cjs');
  esbuild.buildSync({entryPoints:[path.resolve(entry)],outfile,bundle:true,platform:'node',format:'cjs',target:'node22',external:['jsdom'],nodePaths:[path.join(__dirname,'tools/node_modules')],logLevel:'silent'});
  return await run(outfile,b);
 }finally{fs.rmSync(b,{recursive:true,force:true});}
};
