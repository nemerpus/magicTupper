'use strict';
const fs=require('node:fs'),path=require('node:path'),os=require('node:os'),assert=require('node:assert/strict'),crypto=require('node:crypto');
const {appManifest,safeAppPath}=require('./apps');
(async()=>{const root=fs.mkdtempSync(path.join(os.tmpdir(),'magictupper-apps-'));try{
  const app=path.join(root,'APPS','demo');fs.mkdirSync(path.join(app,'assets'),{recursive:true});fs.mkdirSync(path.join(app,'saves'));
  fs.writeFileSync(path.join(app,'demo.nro'),'nro');fs.writeFileSync(path.join(app,'assets','texture.png'),'texture');fs.writeFileSync(path.join(app,'assets','strings.json'),'{}');
  for(const name of ['sources.json','credentials.json','settings.ini','install.log'])fs.writeFileSync(path.join(app,name),'private');fs.writeFileSync(path.join(app,'saves','save.bin'),'save');
  const item={category:'apps',filename:'demo.nro',file:path.join(app,'demo.nro')};const manifest=await appManifest(root,item);
  assert.equal(manifest.files.length,3);assert.equal(manifest.executable,'demo/demo.nro');assert.equal(manifest.totalBytes,12);assert(manifest.files.some(f=>f.path==='demo/assets/strings.json'));
  assert.equal(manifest.files.find(f=>f.filename==='demo.nro').sha256,crypto.createHash('sha256').update('nro').digest('hex'));
  fs.writeFileSync(path.join(root,'APPS','single.nro'),'single');assert.equal((await appManifest(root,{category:'apps',filename:'single.nro',file:path.join(root,'APPS','single.nro')})).files.length,1);
  await assert.rejects(appManifest(root,{...item,category:'games'}));
  const linked=path.join(root,'APPS','linked');fs.symlinkSync(app,linked,'junction');await assert.rejects(appManifest(root,{...item,file:path.join(linked,'demo.nro')}));fs.unlinkSync(linked);
  assert(!safeAppPath('demo/bad?.txt'));assert(!safeAppPath('../other.nro'));
  console.log('PASS: App manifests, resource structure, hashes, standalone NRO, private exclusions and unsafe paths');
}finally{fs.rmSync(root,{recursive:true,force:true});}})().catch(e=>{console.error(e);process.exitCode=1;});
