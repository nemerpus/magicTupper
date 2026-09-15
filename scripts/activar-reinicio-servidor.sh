#!/bin/sh
set -eu
docker update --restart unless-stopped magictupper
docker inspect magictupper --format 'Reinicio={{.HostConfig.RestartPolicy.Name}} Estado={{.State.Status}}'
