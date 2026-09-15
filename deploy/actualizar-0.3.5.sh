#!/bin/sh
set -eu
task_package=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$task_package"
sha256sum -c SHA256SUMS.txt
task_root=/srv/docker/magictupper
test "$(docker inspect magictupper --format '{{range .Mounts}}{{if eq .Destination "/app/server"}}{{.Source}}{{end}}{{end}}')" = "$task_root/server"
test "$(docker inspect magictupper --format '{{range .Mounts}}{{if eq .Destination "/config"}}{{.Source}}{{end}}{{end}}')" = "$task_root/config"
test -f "$task_root/config/config.json"
test -d "$task_root/data"
docker exec magictupper node -e 'const fs=require("fs");for(const name of ["ROMS","DLC","UPDATES","APPS"])if(!fs.statSync("/library/NINTENDO SWITCH/"+name).isDirectory())process.exit(1)'
task_browser=$(docker exec magictupper node -e 'const fs=require("fs");for(const p of ["/games","/reconvertido/juegos"]){try{if(fs.statSync(p).isDirectory()){console.log(p);process.exit(0)}}catch{}}console.error("Falta montar juegos en /games. Revisa compose_magictupper.yml antes de actualizar.");process.exit(1)')
task_image=$(docker inspect magictupper --format '{{.Config.Image}}')
task_backup=$(mktemp -d "$task_root/backup-0.3.5-XXXXXXXX")
chmod 700 "$task_backup"
docker stop magictupper
# Take a consistent snapshot before modifying code or configuration.
if ! cp -a "$task_root/server" "$task_root/config" "$task_root/data" "$task_backup/"; then
    docker start magictupper
    exit 1
fi
task_complete=0
rollback() {
    task_result=$?
    if [ "$task_complete" != 1 ]; then
        echo "Actualizacion fallida. Restaurando codigo y configuracion desde $task_backup" >&2
        docker stop magictupper >/dev/null 2>&1 || true
        cp -a "$task_backup/server/." "$task_root/server/"
        cp -a "$task_backup/config/." "$task_root/config/"
        docker start magictupper || true
    fi
    trap - EXIT
    exit "$task_result"
}
trap rollback EXIT
cp server/server.js server/users.js server/library.js "$task_root/server/"
cp -a server/admin/. "$task_root/server/admin/"
docker run --rm --pull never --network none -v "$task_root/config:/config" -e "MT_BROWSER=$task_browser" "$task_image" node -e 'const fs=require("fs"),p="/config/config.json",c=JSON.parse(fs.readFileSync(p,"utf8").replace(/^\uFEFF/,""));c.library="/library/NINTENDO SWITCH";c.browserRoot=process.env.MT_BROWSER;c.catalogMode="switch";fs.writeFileSync(p,JSON.stringify(c,null,2)+"\n")'
docker update --restart unless-stopped magictupper
docker start magictupper
task_healthy=0
for task_attempt in 1 2 3 4 5 6 7 8 9 10; do
    if docker exec magictupper node -e 'fetch("http://127.0.0.1:8765/health").then(r=>process.exit(r.ok?0:1)).catch(()=>process.exit(1))'; then task_healthy=1;break;fi
    sleep 2
done
test "$task_healthy" = 1
task_complete=1
printf 'Actualizado a 0.3.5. Respaldo: %s\n' "$task_backup"
docker inspect magictupper --format 'Estado={{.State.Status}} Reinicio={{.HostConfig.RestartPolicy.Name}}'
