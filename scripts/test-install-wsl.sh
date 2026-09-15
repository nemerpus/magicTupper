#!/bin/sh
set -eu
ASAN_OPTIONS=${ASAN_OPTIONS:-handle_segv=0:abort_on_error=1}
export ASAN_OPTIONS
project=${1:?Falta ruta del proyecto}
output="$project/.tools/installer-tests"
mkdir -p "$output"
cxx=${CXX:-g++}
includes=/var/cache/magictupper-devkit/bundle/rootfs/opt/devkitpro/portlibs/switch/include
for test in package cnmt; do
    "$cxx" -std=c++17 -Wall -Wextra -Werror -fno-pie -no-pie -fsanitize=address,undefined -g "$project/switch/tests/$test.cpp" -o "$output/$test"
    "$output/$test"
done
"$cxx" -std=c++17 -Wall -Wextra -Werror -fno-pie -no-pie -fsanitize=address,undefined -g -I"$includes" "$project/switch/tests/http_range.cpp" -Wl,-l:libcurl.so.4 -o "$output/http-range"
python3 "$project/switch/tests/http_range_server.py" "$output/http-range"
"$cxx" -std=c++17 -Wall -Wextra -Werror -Wno-missing-field-initializers -fno-pie -no-pie -fsanitize=address,undefined -g -I"$includes" "$project/switch/tests/jobs.cpp" -Wl,-l:libjson-c.so.5 -o "$output/jobs"
"$output/jobs" "$output/jobs-test.json"
"$cxx" -std=c++17 -Wall -Wextra -Werror -fno-pie -no-pie -fsanitize=address,undefined -g "$project/switch/tests/local_file.cpp" -Wl,--wrap=fstat -o "$output/local-file"
"$output/local-file" /tmp/magictupper-local-file-test.nsp
"$cxx" -std=c++17 -Wall -Wextra -Werror -Wno-unused-function -Wno-missing-field-initializers -fno-pie -no-pie -fsanitize=address,undefined -g -I"$includes" "$project/switch/tests/explorer.cpp" -Wl,-l:libjson-c.so.5 -o "$output/explorer"
"$output/explorer"
"$cxx" -std=c++17 -Wall -Wextra -Werror -Wno-unused-function -Wno-missing-field-initializers -fno-pie -no-pie -fsanitize=address,undefined -g -I"$includes" "$project/switch/tests/updates.cpp" -Wl,-l:libjson-c.so.5 -o "$output/updates"
"$output/updates"
"$cxx" -std=c++17 -Wall -Wextra -Werror -Wno-unused-function -Wno-missing-field-initializers -fno-pie -no-pie -fsanitize=address,undefined -g -I"$includes" "$project/switch/tests/apps.cpp" -Wl,-l:libjson-c.so.5 -o "$output/apps"
"$output/apps"
"$cxx" -std=c++17 -Wall -Wextra -Werror -Wno-unused-function -Wno-missing-field-initializers -fno-pie -no-pie -fsanitize=address,undefined -g -I"$includes" "$project/switch/tests/https.cpp" -Wl,-l:libcurl.so.4 -Wl,-l:libjson-c.so.5 -o "$output/https"
python3 "$project/switch/tests/https_server.py" "$output/https"
