'use strict';
const $=id=>document.getElementById(id);
let token='',me=null,passwordTarget=null,busy=false;
function notice(message,error=false){$('notice').textContent=message;$('notice').classList.toggle('error',error);}
function signedOut(){token='';me=null;$('management').hidden=true;$('logout').hidden=true;$('loginSection').hidden=false;if($('passwordDialog').open)$('passwordDialog').close();}
async function api(url,method='GET',body){const response=await fetch(url,{method,headers:{'Content-Type':'application/json',...(token?{Authorization:'Bearer '+token}:{})},...(body!==undefined?{body:JSON.stringify(body)}:{})});const data=await response.json();if(!response.ok){if(response.status===401)signedOut();throw new Error(data.error||'No se pudo completar la operación.');}return data;}
async function run(operation){if(busy)return;busy=true;document.querySelectorAll('button,select').forEach(e=>e.disabled=true);try{await operation();}catch(error){notice(error.message,true);}finally{busy=false;document.querySelectorAll('button,select').forEach(e=>e.disabled=e.dataset.locked==='true');}}
function button(label,action,style='secondary'){const element=document.createElement('button');element.type='button';element.textContent=label;element.className=style;element.addEventListener('click',()=>run(action));return element;}
function cell(row,value){const td=document.createElement('td');td.textContent=value;row.append(td);return td;}
async function updated(id,message){if(me&&id===me.id){signedOut();notice(message+' Vuelve a iniciar sesión.');}else{await refresh();notice(message);}}
async function refresh(){
  const data=await api('/api/admin/users');const fragment=document.createDocumentFragment();
  for(const user of data.users){
    const row=document.createElement('tr');cell(row,user.username+(user.id==='owner'?' · principal':''));
    const role=cell(row,'');const select=document.createElement('select');select.setAttribute('aria-label','Permisos de '+user.username);
    for(const value of ['standard','admin']){const option=document.createElement('option');option.value=value;option.textContent=value==='admin'?'Admin':'Estándar';select.append(option);}select.value=user.role==='user'?'standard':user.role;
    if(user.id==='owner'){select.disabled=true;select.dataset.locked='true';}
    select.addEventListener('change',()=>run(async()=>{const role=select.value;try{await api('/api/admin/users/'+user.id,'PATCH',{role});await updated(user.id,'Permisos actualizados.');}catch(error){select.value=user.role;throw error;}}));role.append(select);
    cell(row,user.enabled?'Activo':'Desactivado');cell(row,String(user.activeSessions));const actions=cell(row,'');actions.className='actions';
    const toggle=button(user.enabled?'Desactivar':'Activar',async()=>{await api('/api/admin/users/'+user.id,'PATCH',{enabled:!user.enabled});await updated(user.id,user.enabled?'Cuenta desactivada.':'Cuenta activada.');},user.enabled?'danger':'secondary');if(user.id==='owner'){toggle.disabled=true;toggle.dataset.locked='true';}actions.append(toggle);
    actions.append(button('Contraseña',async()=>{passwordTarget=user;$('passwordUser').textContent=user.username;$('passwordForm').reset();$('passwordDialog').showModal();$('passwordForm').elements.password.focus();}));
    actions.append(button('Cerrar sesiones',async()=>{await api('/api/admin/users/'+user.id+'/revoke','POST',{});await updated(user.id,'Sesiones cerradas.');}));
    const remove=button('Eliminar',async()=>{if(!confirm('¿Eliminar definitivamente al usuario '+user.username+'? Esta acción cerrará sus sesiones.'))return;await api('/api/admin/users/'+user.id,'DELETE');await refresh();notice('Usuario '+user.username+' eliminado.');},'danger');if(user.id==='owner'){remove.disabled=true;remove.dataset.locked='true';remove.title='La cuenta principal no se puede eliminar.';}actions.append(remove);fragment.append(row);
  }
  $('users').replaceChildren(fragment);
}
$('loginForm').addEventListener('submit',event=>{event.preventDefault();run(async()=>{const form=event.currentTarget;const password=form.elements.password.value;form.elements.password.value='';const result=await api('/api/login','POST',{username:form.elements.username.value,password});token=result.token;me=result.user;if(!me||me.role!=='admin'){await api('/api/logout','POST',{});signedOut();throw new Error('Esta cuenta puede usar la tienda, pero no administrar usuarios.');}await refresh();$('loginSection').hidden=true;$('management').hidden=false;$('logout').hidden=false;notice('Sesión de '+me.username+'.');});});
$('createForm').addEventListener('submit',event=>{event.preventDefault();const form=event.currentTarget;run(async()=>{await api('/api/admin/users','POST',{username:form.elements.username.value,password:form.elements.password.value,role:form.elements.role.value});form.reset();await refresh();notice('Usuario creado. Ya puede acceder desde la Switch.');});});
$('passwordForm').addEventListener('submit',event=>{event.preventDefault();const form=event.currentTarget;run(async()=>{const id=passwordTarget.id;await api('/api/admin/users/'+id,'PATCH',{password:form.elements.password.value});form.reset();$('passwordDialog').close();await updated(id,'Contraseña actualizada y sesiones cerradas.');});});
$('cancelPassword').addEventListener('click',()=>{$('passwordForm').reset();$('passwordDialog').close();});
$('refresh').addEventListener('click',()=>run(async()=>{await refresh();notice('Lista actualizada.');}));
$('logout').addEventListener('click',()=>run(async()=>{try{await api('/api/logout','POST',{});}finally{signedOut();notice('Sesión cerrada.');}}));
