#include <eekernel.h>

int CreateSema(struct SemaParam *param) {
    PS2_UNIMPLEMENTED();
}

int DeleteSema(int sema_id) {
    PS2_UNIMPLEMENTED();
}

int WaitSema(int sema_id) {
    PS2_UNIMPLEMENTED();
}

int SignalSema(int sema_id) {
    PS2_UNIMPLEMENTED();
}

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

int CreateThread(struct ThreadParam *param) {
    PS2_UNIMPLEMENTED();
}

int StartThread(int thread_id, void *arg) {
    PS2_UNIMPLEMENTED();
}

int RotateThreadReadyQueue(int priority) {
    PS2_UNIMPLEMENTED();
}

int TerminateThread(int thread_id) {
    PS2_UNIMPLEMENTED();
}

int DeleteThread(int thread_id) {
    PS2_UNIMPLEMENTED();
}
