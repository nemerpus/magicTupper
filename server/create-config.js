'use strict';
const fs=require('node:fs');
const path=require('node:path');
const crypto=require('node:crypto');
const {hash}=require('./server');
const {usernameValid,passwordValid}=require('./users');
async function createConfig(values,output) {
  if(typeof values.name!=='string'||!values.name.trim())throw new Error('Falta el nombre.');
  usernameValid(values.username);passwordValid(values.password);
  const config=JSON.parse(fs.readFileSync(path.join(__dirname,'config.example.json'),'utf8'));
  Object.assign(config,{name:values.name,username:values.username,library:'/library',dataDir:'/data',passwordSalt:crypto.randomBytes(16).toString('hex')});
  config.passwordHash=await hash(values.password,config.passwordSalt);
  fs.mkdirSync(path.dirname(output),{recursive:true});
  fs.writeFileSync(output,JSON.stringify(config,null,2)+'\n',{mode:0o600});
}
if(require.main===module){
  let body='';process.stdin.setEncoding('utf8');process.stdin.on('data',part=>{body+=part;});
  process.stdin.on('end',()=>{
    Promise.resolve().then(()=>createConfig(JSON.parse(body.replace(/^\uFEFF/,'')),path.resolve(process.argv[2]||'deploy/config/config.json')))
      .then(()=>console.log('Configuración creada para contenedor; biblioteca /library.'))
      .catch(error=>{console.error(error.message);process.exitCode=1;});
  });
}
module.exports={createConfig};
