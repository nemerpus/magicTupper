'use strict';
const fs=require('node:fs');
const path=require('node:path');
const crypto=require('node:crypto');
const {promisify}=require('node:util');
const derive=promisify(crypto.pbkdf2);
async function hash(password,salt){return (await derive(password,salt,210000,64,'sha512')).toString('hex');}
function failure(status,message){return Object.assign(new Error(message),{status});}
function passwordValid(password){if(typeof password!=='string'||password.length<6||password.length>256)throw failure(400,'La contraseña necesita entre 6 y 256 caracteres.');}
function usernameValid(username){if(typeof username!=='string'||!username.trim()||username!==username.trim()||username.length>64||/[\x00-\x1f\x7f]/.test(username))throw failure(400,'Usuario inválido: entre 1 y 64 caracteres, sin espacios en los extremos.');}
class Users {
  constructor(config,dir){
    this.file=path.join(dir,'users.json');this.pending=Promise.resolve();
    fs.mkdirSync(dir,{recursive:true});
    const fingerprint=crypto.createHash('sha256').update(JSON.stringify([config.username,config.passwordSalt,config.passwordHash])).digest('hex');
    this.state=fs.existsSync(this.file)?JSON.parse(fs.readFileSync(this.file,'utf8')):{version:1,users:[]};
    if(this.state.version!==1||!Array.isArray(this.state.users))throw new Error('Archivo de usuarios inválido. Restaura una copia de seguridad.');
    let migrated=false;
    for(const user of this.state.users){
      if(user&&user.role==='user'){user.role='standard';migrated=true;}
      if(!user||typeof user.id!=='string'||typeof user.username!=='string'||!['admin','standard'].includes(user.role)||typeof user.enabled!=='boolean'||!Number.isSafeInteger(user.sessionVersion)||!Number.isSafeInteger(user.version)||!/^[0-9a-f]{32}$/.test(user.passwordSalt)||!/^[0-9a-f]{128}$/.test(user.passwordHash))throw new Error('Registro de usuario inválido.');
    }
    if(new Set(this.state.users.map(u=>u.id)).size!==this.state.users.length||new Set(this.state.users.map(u=>u.username)).size!==this.state.users.length)throw new Error('Usuarios duplicados en el archivo de datos.');
    if(this.state.bootstrapFingerprint!==fingerprint){
      const owner=this.byId('owner');
      if(this.state.users.some(u=>u.id!=='owner'&&u.username===config.username))throw new Error('El usuario administrador configurado coincide con otra cuenta.');
      const replacement={id:'owner',username:config.username,passwordSalt:config.passwordSalt,passwordHash:config.passwordHash,role:'admin',enabled:true,sessionVersion:(owner?owner.sessionVersion:0)+1,version:(owner?owner.version:0)+1};
      this.state.users=this.state.users.filter(u=>u.id!=='owner').concat(replacement);this.state.bootstrapFingerprint=fingerprint;this.persist(this.state);
    }
    if(!this.byId('owner')||this.byId('owner').role!=='admin'||!this.byId('owner').enabled)throw new Error('Falta el administrador principal activo.');
    if(migrated)this.persist(this.state);
  }
  byId(id){return this.state.users.find(u=>u.id===id);}
  byName(name){return this.state.users.find(u=>u.username===name);}
  persist(state){const temp=this.file+'.tmp';fs.writeFileSync(temp,JSON.stringify(state,null,2)+'\n',{mode:0o600});fs.renameSync(temp,this.file);}
  transact(operation){const result=this.pending.then(operation);this.pending=result.catch(()=>{});return result;}
  async mutate(actorCheck,id,input){return this.transact(async()=>{
    actorCheck();
    if(!input||typeof input!=='object'||Array.isArray(input))throw failure(400,'Datos incorrectos.');
    const next=JSON.parse(JSON.stringify(this.state));
    let user=id?next.users.find(u=>u.id===id):null;
    if(id&&!user)throw failure(404,'Usuario no encontrado.');
    if(!id){
      usernameValid(input.username);passwordValid(input.password);
      if(next.users.some(u=>u.username===input.username))throw failure(409,'Ese usuario ya existe.');
      if(next.users.length>=500)throw failure(409,'Se ha alcanzado el límite de 500 usuarios.');
      user={id:crypto.randomUUID(),username:input.username,enabled:true,role:'standard',sessionVersion:1,version:0};next.users.push(user);
    }
    if(input.role!==undefined){const role=['user','standar'].includes(input.role)?'standard':input.role;if(!['admin','standard'].includes(role))throw failure(400,'Rol inválido.');if(id==='owner'&&role!=='admin')throw failure(409,'No puedes quitar el rol del administrador principal.');user.role=role;}
    if(input.enabled!==undefined){if(typeof input.enabled!=='boolean')throw failure(400,'Estado inválido.');if(id==='owner'&&!input.enabled)throw failure(409,'No puedes desactivar al administrador principal.');user.enabled=input.enabled;}
    if(input.password!==undefined){passwordValid(input.password);user.passwordSalt=crypto.randomBytes(16).toString('hex');user.passwordHash=await hash(input.password,user.passwordSalt);}
    if(id)user.sessionVersion++;user.version++;
    actorCheck(); // Recheck after password derivation: the acting session may have been revoked.
    this.persist(next);this.state=next;return user;
  });}
  revoke(actorCheck,id){return this.transact(()=>{actorCheck();const next=JSON.parse(JSON.stringify(this.state));const user=next.users.find(u=>u.id===id);if(!user)throw failure(404,'Usuario no encontrado.');user.sessionVersion++;user.version++;this.persist(next);this.state=next;return user;});}
  delete(actorCheck,id){return this.transact(()=>{actorCheck();if(id==='owner')throw failure(409,'No puedes eliminar al administrador principal.');const next=JSON.parse(JSON.stringify(this.state));const index=next.users.findIndex(u=>u.id===id);if(index<0)throw failure(404,'Usuario no encontrado.');const [user]=next.users.splice(index,1);actorCheck();this.persist(next);this.state=next;return user;});}
}
module.exports={Users,hash,failure,passwordValid,usernameValid};
