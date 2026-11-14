'use strict';
const fs = require('node:fs'), os = require('node:os'), path = require('node:path');
const { spawnSync } = require('node:child_process');
module.exports = function () {
 const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'needle-author-tls-'));
 try {
  const key = path.join(dir, 'key.pem'), cert = path.join(dir, 'cert.pem');
  const config = path.join(dir, 'openssl.cnf');
  fs.writeFileSync(config, '[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n[dn]\nCN=localhost\n[ext]\nsubjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,digitalSignature,keyEncipherment,keyCertSign\n');
  const result = spawnSync('openssl', ['req','-x509','-newkey','rsa:2048','-nodes','-days','1','-config',config,'-keyout',key,'-out',cert], { encoding: 'utf8', timeout: 15000 });
  if (result.error || result.status !== 0) throw result.error || Error(result.stderr);
  return { key: fs.readFileSync(key), cert: fs.readFileSync(cert) };
 } finally { fs.rmSync(dir, { recursive: true, force: true }); }
};
