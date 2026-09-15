#pragma once

// Protocolo USB bulk de MagicTupper. La implementación del transporte se
// añadirá sobre usbDs/usbComms cuando se valide la enumeración en hardware.
namespace mtusb {
// Se mantiene la versión de trama 1: los comandos nuevos son compatibles con
// el cliente MTUSB1 existente.
constexpr unsigned version = 1;
constexpr char magic[] = "MTUSB1";
enum Command : unsigned char {
    hello = 1, list = 2, stat = 3, read = 4, write = 5,
    makeDirectory = 6, remove = 7, lock = 8, unmount = 9,
    batchBegin = 10, batchFile = 11, batchSelect = 12, batchAction = 13,
    batchDestination = 14, batchCommit = 15, batchStatus = 16, batchResume = 17,
    transferBegin = 0x20, transferData = 0x21, transferEnd = 0x22, transferCancel = 0x23
};
enum BatchState : unsigned char { receiving=1, waitingSelection=2, installing=3, downloading=4, paused=5, completed=6, failed=7, cancelled=8 };
enum BatchAction : unsigned char { actionInstall=1, actionDownload=2, actionCancel=3 };
enum BatchDestination : unsigned char { destinationSd=1, destinationInternal=2, destinationUsb=3 };
#pragma pack(push,1)
struct BatchFileHeader { unsigned char index; unsigned char type; unsigned long long size; unsigned short nameLength; };
struct BatchSelection { unsigned char index; unsigned char selected; };
struct BatchDecision { unsigned char action; unsigned char destination; };
#pragma pack(pop)
}
