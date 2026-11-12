const fs = require('node:fs');
const files=['jsfeat.js','jsfeat_struct.js','jsfeat_cache.js','jsfeat_math.js','jsfeat_mat_math.js','jsfeat_linalg.js','jsfeat_imgproc.js','jsfeat_fast_corners.js','jsfeat_yape06.js','jsfeat_yape.js','jsfeat_orb.js','jsfeat_optical_flow_lk.js','jsfeat_motion_estimator.js','jsfeat_haar.js','jsfeat_bbf.js','jsfeat_export.js'];
if(process.argv.length!==3) throw new Error('usage: node assemble.cjs output');
fs.writeFileSync(process.argv[2],files.map(f=>fs.readFileSync('src/'+f,'utf8')).join('\n'));
