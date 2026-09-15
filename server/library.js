'use strict';
const fs=require('node:fs');
const path=require('node:path');
const crypto=require('node:crypto');

function titleMetadata(filename,category='') {
  const matches=[...filename.matchAll(/(?:^|[^0-9a-f])([0-9a-f]{16})(?=$|[^0-9a-f])/gi)];
  if(matches.length!==1)return {};
  const titleId=matches[0][1].toLowerCase();
  if(!titleId.startsWith('0100'))return {};
  const id=BigInt('0x'+titleId),suffix=id&0xfffn;
  const versions=[...filename.matchAll(/(?:^|[\s\[(])v(\d+)(?=$|[\s\]).])/gi)];
  const value=versions.length===1?Number(versions[0][1]):null;
  const application=category==='dlc'&&(id&0x1fffn)>=0x1000n?((id^0x1000n)&~0xfffn):suffix===0x800n?id-0x800n:id;
  return {titleId,applicationId:application.toString(16).padStart(16,'0'),
    version:Number.isSafeInteger(value)&&value>=0&&value<=0xffffffff?value:null,
    metadataSource:'filename'};
}
function validPath(relative) {
  return typeof relative==='string'&&relative.length<=4096&&!/[\\:\x00-\x1f]/.test(relative)&&
    (relative===''||relative.split('/').every(part=>part&&part!=='.'&&part!=='..'&&!part.startsWith('.')));
}
async function packageMetadata(file,category='') {
  const named=titleMetadata(path.basename(file),category);
  if(path.extname(file).toLowerCase()!=='.nsp')return named;
  let handle;
  try{
    handle=await fs.promises.open(file,'r');const stat=await handle.stat();const head=Buffer.alloc(16);
    if((await handle.read(head,0,16,0)).bytesRead!==16||head.toString('ascii',0,4)!=='PFS0')return named;
    const count=head.readUInt32LE(4),strings=head.readUInt32LE(8),length=count*24+strings;
    if(count>4096||length>8*1024*1024||16+length>stat.size)return named;
    const index=Buffer.alloc(length);if((await handle.read(index,0,length,16)).bytesRead!==length)return named;
    const ids=new Set();
    for(let i=0;i<count;i++){
      const at=i*24,offset=index.readBigUInt64LE(at),size=index.readBigUInt64LE(at+8),start=count*24+index.readUInt32LE(at+16);
      if(start>=length||offset+size>BigInt(stat.size-16-length))continue;
      const end=index.indexOf(0,start);if(end<0)continue;
      const name=index.toString('ascii',start,end),match=/^([0-9a-f]{16})[0-9a-f]{16}\.tik$/i.exec(name);
      if(match)ids.add(match[1].toLowerCase());
    }
    if(ids.size>1)return {};
    if(ids.size===1){
      const id=[...ids][0];if(named.titleId&&named.titleId!==id)return {};
      const metadata=titleMetadata('['+id+']',category);return {...metadata,version:named.version??null,metadataSource:'ticket-filename'};
    }
    return named;
  }catch(error){if(['ENOENT','EACCES','EIO'].includes(error.code))return {};throw error;}
  finally{if(handle)await handle.close();}
}
async function browseDirectory(root,relative) {
  if(!validPath(relative))throw Object.assign(new Error('Ruta no permitida.'),{status:400});
  root=await fs.promises.realpath(root);
  let directory=root;
  for(const part of relative?relative.split('/'):[]) {
    directory=path.join(directory,part);
    const stat=await fs.promises.lstat(directory);
    if(stat.isSymbolicLink()||!stat.isDirectory())throw Object.assign(new Error('Carpeta no permitida.'),{status:403});
  }
  const real=await fs.promises.realpath(directory),rel=path.relative(root,real);
  if(rel==='..'||rel.startsWith('..'+path.sep)||path.isAbsolute(rel))throw Object.assign(new Error('Ruta no permitida.'),{status:403});
  const entries=[];
  // Read one directory only: browsing the retro collection never indexes its entire tree.
  const names=await fs.promises.readdir(real,{withFileTypes:true});
  if(names.length>10000)throw Object.assign(new Error('Carpeta demasiado grande; divide su contenido en subcarpetas.'),{status:422});
  for(const name of names) {
    if(name.name.startsWith('.')||name.isSymbolicLink())continue;
    const file=path.join(real,name.name),relativePath=relative?relative+'/'+name.name:name.name;
    if(!validPath(relativePath))continue;
    let stat;try{stat=await fs.promises.lstat(file);}catch(error){if(['ENOENT','EACCES','EIO'].includes(error.code))continue;throw error;}
    if(stat.isSymbolicLink()||(!stat.isDirectory()&&!stat.isFile()))continue;
    const id=stat.isFile()?crypto.createHash('sha256').update('browser:'+relativePath+':'+stat.size+':'+stat.mtime.getTime()).digest('hex'):'';
    entries.push({id,name:name.name,relativePath,directory:stat.isDirectory(),size:stat.isFile()?stat.size:0,file,mtime:stat.mtime.getTime(),root});
  }
  return entries.sort((a,b)=>Number(b.directory)-Number(a.directory)||a.name.localeCompare(b.name));
}
module.exports={titleMetadata,packageMetadata,validPath,browseDirectory};
