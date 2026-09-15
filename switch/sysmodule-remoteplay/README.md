# magicTupper Remote Play sysmodule (rc.5)

Servicio de Atmosphère que recibe MTRP v1 por UDP/8767 y crea un mando Pro virtual mediante `hid:dbg`/HDLS.

- Se adjunta el mando virtual al recibir el primer paquete válido.
- A 250 ms sin paquetes se envía estado neutro para evitar botones atascados.
- A 2 s sin paquetes se desconecta el mando virtual.
- El ACK mantiene el formato de Remote Play LAB rc.3/rc.4, por lo que el PC Client sigue midiendo RTT.

Compilar con `make` usando devkitPro/libnx actual. El `.nsp` resultante se instala como `exefs.nsp` bajo el title ID `42000000004D5452`.
