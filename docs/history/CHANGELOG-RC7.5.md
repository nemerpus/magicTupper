# magicTupper rc7.5 — HOME pre-transfer probe

## Why
rc7.4 proved START/OK works. The PC status `SIN RESPUESTA` means the initial OK was received, but the sysmodule then stops emitting packets. The log shows `grc:d Begin OK` followed by silence.

`mtGrcdTransfer()` is a synchronous IPC call and can block while HOME is displayed, therefore the 700 ms no-frame detector placed after that call cannot execute.

## Change
Immediately after a successful `grc:d Begin`, rc7.5 performs one diagnostic HOME `caps:sc` probe before entering the blocking video transfer. This lets us obtain the real caps:sc result code without changing the known-good GRC/RTP game pipeline.
