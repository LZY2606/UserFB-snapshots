const path=require('node:path');
const {buildSync}=require('./tools/node_modules/esbuild');
const tools=path.resolve(__dirname,'tools/node_modules');
exports.environment={...process.env,NODE_PATH:tools};
exports.bundle=(entry,outfile)=>buildSync({entryPoints:[entry],outfile,bundle:true,platform:'node',format:'cjs',packages:'external',target:'node22',nodePaths:[tools],logLevel:'warning'});
