#include <eekernel.h>

int DIntr(void) {
    PS2_UNIMPLEMENTED();
}

int EIntr(...) {
    PS2_UNIMPLEMENTED();
}

int AddIntcHandler(int cause, int (*handler)(int), int next) {
    PS2_UNIMPLEMENTED();
}

int RemoveIntcHandler(int cause, int handler) {
    PS2_UNIMPLEMENTED();
}

int EnableIntc(int cause) {
    PS2_UNIMPLEMENTED();
}

int AddDmacHandler(int channel, int (*handler)(int), int next) {
    PS2_UNIMPLEMENTED();
}

int RemoveDmacHandler(int channel, int handler) {
    PS2_UNIMPLEMENTED();
}

int EnableDmac(int channel) {
    PS2_UNIMPLEMENTED();
}

int DisableDmac(int channel) {
    PS2_UNIMPLEMENTED();
}
