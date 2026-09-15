FROM devkitpro/devkita64:20260219
RUN dkp-pacman -Syu --noconfirm && dkp-pacman -S --needed --noconfirm switch-sdl2 switch-sdl2_ttf switch-curl switch-libjson-c switch-pkg-config
WORKDIR /src
CMD ["make", "-j2"]
