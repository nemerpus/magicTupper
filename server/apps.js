'use strict';
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {validPath}=require('./library');
const blocked=new Set(['downloads','cache','saves','logs','source','tests']);
function safeAppPath(value){return validPath(value)&&value.length<=220&&value.split('/').every(p=>!/[<>"|?*]/.test(p)&&!/[ .]$/.test(p))&&!value.split('/').some(p=>blocked.has(p.toLowerCase()));}
function personal(name){return /^(sources|queue|config|credentials|accounts|tokens|sessions|settings|users)(\.|$)/i.test(name)||/\.(log|bak|part|keys|db)$/i.test(name);}
async function appManifest(root,item){
  const relative=path.relative(root,item.file).split(path.sep).join('/'),parts=relative.split('/');
  if(item.category!=='apps'||!item.filename.toLowerCase().endsWith('.nro')||parts[0].toLowerCase()!=='apps')throw Object.assign(new Error('Selecciona un NRO de APPS.'),{status:400});
  const base=path.join(root,parts[0]),folder=parts.length>2?parts[1]:null,files=[];let plannedBytes=0;
  async function confined(file){const real=await fs.promises.realpath(file),rel=path.relative(root,real);if(rel==='..'||rel.startsWith('..'+path.sep)||path.isAbsolute(rel))throw Object.assign(new Error('Recurso fuera de APPS.'),{status:422});}
  async function add(file,dest){
    if(!safeAppPath(dest))throw Object.assign(new Error('Ruta no válida en la App.'),{status:422});
    const stat=await fs.promises.lstat(file);if(!stat.isFile()||stat.isSymbolicLink())throw Object.assign(new Error('La App contiene un enlace o archivo no permitido.'),{status:422});
    await confined(file);plannedBytes+=stat.size;if(plannedBytes>2*1024*1024*1024)throw Object.assign(new Error('App mayor de 2 GiB.'),{status:422});
    if(stat.size>=0x100000000||files.length>=2048)throw Object.assign(new Error('La App supera los límites de archivos.'),{status:422});
    const sha=crypto.createHash('sha256');for await(const chunk of fs.createReadStream(file))sha.update(chunk);
    const digest=sha.digest('hex'),id=crypto.createHash('sha256').update('app:'+dest+':'+digest).digest('hex');
    files.push({path:dest,id,size:stat.size,sha256:digest,file,filename:path.basename(file),mtime:stat.mtime.getTime(),root});
  }
  async function walk(dir,prefix){
    await confined(dir);if(prefix.split('/').length>24)throw Object.assign(new Error('Demasiadas subcarpetas en la App.'),{status:422});
    for(const entry of await fs.promises.readdir(dir,{withFileTypes:true})){
      if(entry.name.startsWith('.')||blocked.has(entry.name.toLowerCase())||personal(entry.name))continue;
      if(entry.isSymbolicLink())throw Object.assign(new Error('La App contiene un enlace.'),{status:422});
      const file=path.join(dir,entry.name),dest=prefix+'/'+entry.name;
      if(entry.isDirectory())await walk(file,dest);else if(entry.isFile())await add(file,dest);
    }
  }
  if(folder){const stat=await fs.promises.lstat(path.join(base,folder));if(stat.isSymbolicLink())throw Object.assign(new Error('Carpeta enlazada no permitida.'),{status:422});await walk(path.join(base,folder),folder);}
  else await add(item.file,item.filename);
  const executable=parts.slice(1).join('/');
  if(!files.some(f=>f.path===executable))throw Object.assign(new Error('Falta el ejecutable de la App.'),{status:422});
  const names=new Set();for(const file of files){const key=file.path.toLowerCase();if(names.has(key))throw Object.assign(new Error('Rutas duplicadas para la SD.'),{status:422});names.add(key);}
  const totalBytes=files.reduce((sum,f)=>sum+f.size,0);if(totalBytes>2*1024*1024*1024)throw Object.assign(new Error('App mayor de 2 GiB.'),{status:422});
  return {version:1,executable,totalBytes,files};
}
module.exports={appManifest,safeAppPath};
