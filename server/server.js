'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const {Users,failure,passwordValid,usernameValid}=require('./users');
const {packageMetadata,browseDirectory}=require('./library');
const {appManifest}=require('./apps');
const TYPES = { roms:'games', dlc: 'dlc', updates: 'updates', retro: 'retro', apps: 'apps', 'nintendo switch': 'games', 'nintendo switch apps': 'apps' };
const EXT = new Set(['.nsp', '.nsz', '.xci', '.xcz', '.nro', '.zip', '.nes', '.sfc', '.smc', '.gba', '.gb', '.gbc', '.n64', '.z64', '.nds', '.iso', '.cso', '.chd', '.cue', '.bin']);
function hash(password, salt) {
  return new Promise(function(resolve, reject) {
    crypto.pbkdf2(password, salt, 210000, 64, 'sha512', function(err, key) { if (err) reject(err); else resolve(key.toString('hex')); });
  });
}
function inside(root, file) { const rel = path.relative(root, file); return rel !== '' && rel !== '..' && !rel.startsWith('..' + path.sep) && !path.isAbsolute(rel); }
function scan(root) {
  root=fs.realpathSync(root);
  const entries = [];
  function categoryFor(name, inherited) {
    const normalized = name.toLowerCase().replace(/\s+/g, ' ').trim();
    return TYPES[normalized] || inherited || 'retro';
  }
  function walk(dir, category) {
    fs.readdirSync(dir).sort().forEach(function(name) {
      if (name.startsWith('.')) return;
      const file = path.join(dir, name), stat = fs.lstatSync(file);
      if (stat.isSymbolicLink() || !inside(root, fs.realpathSync(file))) return;
      if (stat.isDirectory()) { walk(file, categoryFor(name, category)); return; }
      if (!stat.isFile() || !EXT.has(path.extname(name).toLowerCase())) return;
      const relative = path.relative(root, file);
      const id = crypto.createHash('sha256').update(relative + ':' + stat.size + ':' + stat.mtime.getTime()).digest('hex');
      entries.push({id: id, title: path.basename(name, path.extname(name)), filename: name, category: category || 'retro', size: stat.size, mtime: stat.mtime.getTime(), file: file});
    });
  }
  walk(root, null);
  return entries;
}
async function scanAsync(root, switchOnly=false) {
  root=await fs.promises.realpath(root);
  const entries = [];
  function categoryFor(name, inherited) {
    const normalized = name.toLowerCase().replace(/\s+/g, ' ').trim();
    return TYPES[normalized] || inherited || 'retro';
  }
  async function walk(dir, category) {
    const names = (await fs.promises.readdir(dir)).sort();
    for (const name of names) {
      if (name.startsWith('.')) continue;
      const file = path.join(dir, name), stat = await fs.promises.lstat(file);
      if (stat.isSymbolicLink() || !inside(root, await fs.promises.realpath(file))) continue;
      if (stat.isDirectory()) {
        if(switchOnly&&dir===root&&!['roms','dlc','updates','apps'].includes(name.toLowerCase()))continue;
        await walk(file, categoryFor(name, category)); continue;
      }
      if (!stat.isFile() || !EXT.has(path.extname(name).toLowerCase())) continue;
      if (switchOnly && !(['games','dlc','updates','apps'].includes(category) && (category==='apps'?['.nro','.zip']:['.nsp','.xci','.nsz','.xcz']).includes(path.extname(name).toLowerCase()))) continue;
      const relative = path.relative(root, file);
      const id = crypto.createHash('sha256').update(relative + ':' + stat.size + ':' + stat.mtime.getTime()).digest('hex');
      entries.push({id, title:path.basename(name, path.extname(name)), filename:name, category:category || 'retro', size:stat.size, mtime:stat.mtime.getTime(), file, metadata:await packageMetadata(file,category)});
    }
  }
  await walk(root, null);
  return entries;
}
function createShop(config, configDir) {
  if (!config.username || !/^[0-9a-f]{128}$/.test(config.passwordHash) || !/^[0-9a-f]{32}$/.test(config.passwordSalt)) throw new Error('Ejecuta Configurar.ps1 para crear el usuario.');
  const root = fs.realpathSync(path.resolve(configDir, config.library));
  const browserRoot=fs.realpathSync(path.resolve(configDir,config.browserRoot||config.library));
  const browserFiles=new Map();
  if (!fs.statSync(root).isDirectory()) throw new Error('La biblioteca debe ser una carpeta.');
  const users=new Users(config,path.resolve(configDir,config.dataDir||'data'));
  const sessions = new Map(), attempts = new Map();
  function currentSession(token){const session=sessions.get(token);if(!session)return null;const user=users.byId(session.userId);if(session.expires<=Date.now()||!user||!user.enabled||user.sessionVersion!==session.version){for(const response of session.transfers)response.destroy();sessions.delete(token);return null;}return {session,user};}
  function revokeUser(id){for(const [token,session] of sessions)if(session.userId===id){for(const response of session.transfers)response.destroy();sessions.delete(token);}}
  function publicUser(user){let count=0;for(const [token] of sessions){const entry=currentSession(token);if(entry&&entry.user.id===user.id)count++;}return {id:user.id,username:user.username,role:user.role,enabled:user.enabled,activeSessions:count};}
  function readBody(req){return new Promise((resolve,reject)=>{let size=0;const chunks=[];req.on('data',chunk=>{size+=chunk.length;if(size>4096){reject(failure(413,'Solicitud demasiado grande.'));return;}chunks.push(chunk);});req.on('end',()=>{try{resolve(JSON.parse(Buffer.concat(chunks).toString('utf8')));}catch(error){reject(failure(400,'JSON incorrecto.'));}});req.on('error',reject);});}
  let catalog = [], lastScan = 0, scanInFlight, scanError;
  function refresh() {
    if (scanInFlight || (lastScan && Date.now() - lastScan <= 10000)) return;
    scanInFlight = scanAsync(root,config.catalogMode==='switch').then(function(next) {
      catalog = next; lastScan = Date.now(); scanError = null;
    }).catch(function(error) {
      scanError = error;
      console.error('[catalog] scan failed '+JSON.stringify({code:error.code,syscall:error.syscall,path:error.path,message:error.message}));
    }).finally(function() { scanInFlight = null; });
  }
  refresh();
  function json(res, status, body) { res.writeHead(status, {'Content-Type':'application/json; charset=utf-8'}); res.end(JSON.stringify(body)); }
  const server = http.createServer(function(req, res) {
    res.setHeader('Cache-Control', 'no-store');
    res.setHeader('X-Content-Type-Options', 'nosniff');
    const route = req.url.split('?')[0];
    if (req.method === 'GET' && route === '/health') return json(res, 200, {ok:true, service:'MagicTupper'});
    const assets={'/admin':'admin.html','/admin/':'admin.html','/admin/admin.js':'admin.js','/admin/admin.css':'admin.css'};
    if(req.method==='GET'&&assets[route]){
      res.setHeader('Content-Security-Policy',"default-src 'self'; script-src 'self'; style-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
      const file=assets[route];res.setHeader('Content-Type',file.endsWith('.html')?'text/html; charset=utf-8':file.endsWith('.css')?'text/css; charset=utf-8':'text/javascript; charset=utf-8');
      try{res.end(fs.readFileSync(path.join(__dirname,'admin',file)));}catch(error){json(res,500,{error:'No se encuentra el panel de administración.'});}return;
    }
    if (req.method === 'POST' && route === '/api/login') {
      const ip = req.socket.remoteAddress, now = Date.now();
      attempts.forEach(function(value, key) { if (value.until <= now) attempts.delete(key); });
      const attempt = attempts.get(ip) || {count:0, until:now + 60000};
      if (attempt.count >= 5 || attempts.size >= 1024) return json(res, 429, {error:'Demasiados intentos. Espera un minuto.'});
      attempt.count++; attempts.set(ip, attempt);
      let body = '', rejected = false;
      req.on('data', function(chunk) { if (rejected) return; body += chunk; if (Buffer.byteLength(body) > 4096) { rejected = true; json(res,413,{error:'Solicitud demasiado grande'}); } });
      req.on('end', function() {
        if (rejected) return;
        let input;
        try { input = JSON.parse(body); } catch (err) { return json(res,400,{error:'JSON incorrecto'}); }
        if (!input || typeof input.username !== 'string' || typeof input.password !== 'string' || input.password.length > 256) return json(res,400,{error:'Credenciales incorrectas'});
        const account=users.byName(input.username),revision=account&&account.version;
        hash(input.password, account?account.passwordSalt:config.passwordSalt).then(function(result) {
          const equal = crypto.timingSafeEqual(Buffer.from(result,'hex'), Buffer.from(account?account.passwordHash:config.passwordHash,'hex'));
          const fresh=account&&users.byId(account.id);
          if (!equal || !fresh || !fresh.enabled || fresh.version!==revision) return json(res,401,{error:'Credenciales incorrectas'});
          sessions.forEach(function(value,key) { currentSession(key); });
          if (sessions.size >= 128) return json(res,503,{error:'Demasiadas sesiones activas'});
          const token = crypto.randomBytes(32).toString('hex');
          sessions.set(token, {expires:Date.now() + 12 * 60 * 60 * 1000,userId:fresh.id,version:fresh.sessionVersion,transfers:new Set()});
          json(res,200,{token:token, expiresIn:43200,user:publicUser(fresh)});
        }).catch(function() { json(res,500,{error:'No se pudo iniciar sesión'}); });
      });
      return;
    }
    const token = (req.headers.authorization || '').replace(/^Bearer /, '');
    const authenticated=currentSession(token);
    if (!authenticated) return json(res,401,{error:'Inicia sesión'});
    if(authenticated.user.role!=='admin'&&!['GET','HEAD'].includes(req.method)&&route!=='/api/logout')return json(res,403,{error:'El rol estándar solo puede consultar, descargar e instalar.'});
    if (req.method === 'POST' && route === '/api/logout') {for(const response of authenticated.session.transfers)response.destroy();sessions.delete(token);return json(res,200,{ok:true});}
    if(req.method==='GET'&&route==='/api/me')return json(res,200,{user:publicUser(authenticated.user)});
    if(route.startsWith('/api/admin/')){
      const adminCheck=()=>{const current=currentSession(token);if(!current)throw failure(401,'Inicia sesión de nuevo.');if(current.user.role!=='admin')throw failure(403,'Solo los administradores pueden gestionar usuarios.');};
      try{adminCheck();}catch(error){return json(res,error.status,{error:error.message});}
      if(req.method==='GET'&&route==='/api/admin/users')return json(res,200,{users:users.state.users.map(publicUser)});
      const match=/^\/api\/admin\/users\/([a-zA-Z0-9-]+)(\/revoke)?$/.exec(route);
      if(req.method==='POST'&&match&&match[2]){users.revoke(adminCheck,match[1]).then(user=>{revokeUser(user.id);json(res,200,{ok:true});}).catch(error=>json(res,error.status||500,{error:error.status?error.message:'No se pudo guardar el cambio.'}));return;}
      if(req.method==='DELETE'&&match&&!match[2]){users.delete(adminCheck,match[1]).then(user=>{revokeUser(user.id);json(res,200,{ok:true,deleted:user.id});}).catch(error=>json(res,error.status||500,{error:error.status?error.message:'No se pudo eliminar el usuario.'}));return;}
      if((req.method==='POST'&&route==='/api/admin/users')||(req.method==='PATCH'&&match&&!match[2])){
        readBody(req).then(input=>users.mutate(adminCheck,match?match[1]:null,input)).then(user=>{revokeUser(user.id);json(res,req.method==='POST'?201:200,{user:publicUser(user)});}).catch(error=>json(res,error.status||500,{error:error.status?error.message:'No se pudo guardar el cambio.'}));return;
      }
      return json(res,404,{error:'Operación no encontrada.'});
    }
    try {
      if(req.method==='GET'&&/^\/api\/apps\/[0-9a-f]{64}$/.test(route)){
        Promise.resolve(scanInFlight).then(async()=>{
          const item=catalog.find(item=>item.id===route.split('/').pop());if(!item)throw Object.assign(new Error('Actualiza el catálogo de Apps.'),{status:404});
          const manifest=await appManifest(root,item);
          for(const file of manifest.files){browserFiles.delete(file.id);browserFiles.set(file.id,file);}
          while(browserFiles.size>20000)browserFiles.delete(browserFiles.keys().next().value);
          json(res,200,{...manifest,files:manifest.files.map(({path,id,size,sha256})=>({path,id,size,sha256}))});
        }).catch(error=>json(res,error.status||503,{error:error.status?error.message:'No se pudo preparar la App.'}));return;
      }
      if(req.method==='GET'&&route==='/api/browse') {
        const relative=new URL(req.url,'http://localhost').searchParams.get('path')||'';
        browseDirectory(browserRoot,relative).then(entries=>{
          for(const entry of entries)if(!entry.directory){browserFiles.delete(entry.id);browserFiles.set(entry.id,{...entry,filename:entry.name});}
          while(browserFiles.size>20000)browserFiles.delete(browserFiles.keys().next().value);
          json(res,200,{path:relative,entries:entries.map(({id,name,relativePath,directory,size})=>({id,name,relativePath,directory,size}))});
        }).catch(error=>json(res,error.status|| (error.code==='ENOENT'?404:503),{error:error.status?error.message:'Carpeta no accesible. Comprueba el montaje del servidor.'}));
        return;
      }
      if (req.method === 'GET' && route === '/api/catalog') {
        refresh();
        const respond=function(){
          if(scanError)return json(res,500,{error:'No se puede indexar la biblioteca. Consulta el registro del servidor.',code:'CATALOG_SCAN_FAILED'});
          json(res,200,{shop:{name:config.name,accent:config.accent,tagline:config.tagline,jokes:config.jokes},items:catalog.map(function(item) { return {id:item.id,title:item.title,filename:item.filename,category:item.category,size:item.size,relativePath:path.relative(root,item.file).split(path.sep).join('/'),...item.metadata}; })});
        };
        return scanInFlight?scanInFlight.then(respond).catch(function(){json(res,500,{error:'No se puede indexar la biblioteca.'});}):respond();
      }
      if ((req.method === 'GET' || req.method === 'HEAD') && /^\/api\/files\/[0-9a-f]{64}$/.test(route)) {
        const entry = catalog.find(function(item) { return item.id === route.split('/').pop(); })||browserFiles.get(route.split('/').pop());
        if (!entry) return json(res,404,{error:'Archivo no encontrado. Actualiza el catálogo.'});
        const real = fs.realpathSync(entry.file);
        if (!inside(entry.root||root, real)) return json(res,403,{error:'Ruta fuera de la biblioteca'});
        const fd = fs.openSync(real,'r');
        const stat = fs.fstatSync(fd);
        if (!stat.isFile() || stat.size !== entry.size || stat.mtime.getTime() !== entry.mtime) { fs.closeSync(fd); return json(res,409,{error:'El archivo cambió. Actualiza el catálogo.'}); }
        let start = 0, end = stat.size - 1, status = 200;
        if (req.headers.range) {
          const match = /^bytes=(\d+)-(\d*)$/.exec(req.headers.range);
          if (match) { start = Number(match[1]); if (match[2]) end = Math.min(end,Number(match[2])); }
          if (!match || !Number.isSafeInteger(start) || !Number.isSafeInteger(end) || start > end || start >= stat.size) {
            fs.closeSync(fd); res.setHeader('Content-Range','bytes */' + stat.size); return json(res,416,{error:'Rango inválido'});
          }
          status = 206; res.setHeader('Content-Range','bytes ' + start + '-' + end + '/' + stat.size);
        }
        res.setHeader('Accept-Ranges','bytes');
        res.setHeader('ETag','"' + entry.id + '"');
        res.setHeader('Content-Type','application/octet-stream');
        res.setHeader('Content-Disposition', "attachment; filename*=UTF-8''" + encodeURIComponent(entry.filename).replace(/['()*]/g,function(c) { return '%' + c.charCodeAt(0).toString(16); }));
        res.setHeader('Content-Length',Math.max(0,end-start+1));
        res.writeHead(status);
        if (req.method === 'HEAD' || stat.size === 0) { fs.closeSync(fd); return res.end(); }
        const stream = fs.createReadStream(real,{fd:fd,autoClose:true,start:start,end:end,highWaterMark:4*1024*1024});
        authenticated.session.transfers.add(res);
        stream.on('error',function(error) {
          console.error('[transfer] read error id='+entry.id+' start='+start+' read='+stream.bytesRead+' expected='+(end-start+1)+' code='+error.code);
          res.destroy();
        });
        res.on('close',function() {
          if(!res.writableFinished) console.error('[transfer] interrupted id='+entry.id+' start='+start+' read='+stream.bytesRead+' expected='+(end-start+1));
          authenticated.session.transfers.delete(res);stream.destroy();
        });
        stream.pipe(res); return;
      }
      json(res,404,{error:'Ruta no encontrada'});
    } catch (err) { if (!res.headersSent) json(res,500,{error:'No se puede leer la biblioteca. Revisa la carpeta en el PC.'}); else res.destroy(); }
  });
  // Las transferencias grandes pueden tener pausas largas por la SD/Wi-Fi.
  // El cliente sigue validando tamaño y cancela explícitamente cuando procede.
  // Una copia de varios GiB puede durar muchos minutos en una biblioteca
  // montada. No cortar el stream por una pausa temporal del disco o red.
  server.timeout = 1800000;
  server.headersTimeout = 60000;
  // This bounds inbound request bodies, not outbound multi-GiB downloads.
  server.requestTimeout = 60000;
  server.maxHeadersCount = 100;
  server.keepAliveTimeout = 5000;
  return server;
}
function configure() {
  let input=''; process.stdin.setEncoding('utf8'); process.stdin.on('data',function(chunk){input+=chunk;});
  process.stdin.on('end',function() {
    let values;
    try { values=JSON.parse(input.replace(/^\uFEFF/,'')); passwordValid(values.password);usernameValid(values.username); }
    catch(err) { console.error(err.message); process.exitCode=1; return; }
    const file=path.join(__dirname,'config.json');
    const config=JSON.parse(fs.readFileSync(fs.existsSync(file)?file:path.join(__dirname,'config.example.json'),'utf8').replace(/^\uFEFF/,''));
    Object.assign(config,{name:values.name,library:values.library,username:values.username,passwordSalt:crypto.randomBytes(16).toString('hex')});
    hash(values.password,config.passwordSalt).then(function(key) { config.passwordHash=key; fs.mkdirSync(config.library,{recursive:true}); fs.writeFileSync(file,JSON.stringify(config,null,2)+'\n'); console.log('Configuración guardada. Reinicia el servidor para aplicarla y revocar las sesiones.'); }).catch(function(err){console.error(err.message);process.exitCode=1;});
  });
}
if (require.main === module) {
  if (process.argv.indexOf('--configure') !== -1) configure();
  else {
    try {
      const file=path.resolve(process.argv[2] || path.join(__dirname,'config.json'));
      const config=JSON.parse(fs.readFileSync(file,'utf8').replace(/^\uFEFF/,''));
      const app=createShop(config,path.dirname(file));
      app.on('error',function(err){console.error(err.message);process.exitCode=1;});
      app.listen(config.port,config.host,function(){console.log(config.name+' escuchando en '+config.host+':'+config.port+' · Ctrl+C para cerrar');});
    } catch(err) { console.error('No se pudo iniciar: '+err.message); process.exitCode=1; }
  }
}
module.exports={createShop:createShop,hash:hash,scan:scan,scanAsync};
