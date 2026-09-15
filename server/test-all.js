'use strict';
const {spawnSync}=require('node:child_process');
const path=require('node:path');
if(Number(process.versions.node.split('.')[0])!==24)throw new Error('Use Node.js 24 LTS.');
for(const file of ['test.js','test-users.js','test-library.js','test-apps.js']){
  const result=spawnSync(process.execPath,[path.join(__dirname,file)],{stdio:'inherit'});
  if(result.error)throw result.error;
  if(result.status!==0)process.exit(result.status||1);
}
