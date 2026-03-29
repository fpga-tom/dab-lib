#include <stdint.h>
#include <stdio.h>

#include "dab_api.h"

void SDRBackend_processSamples(void *context, softbit_complex_t *buf, unsigned int numSamples) {
    (void)context;
    (void)buf;
    (void)numSamples;
}

void SDRBackend_processPassband(void *cbContext, softbit_complex_t *buf, unsigned int numSamples) {
    (void)cbContext;
    (void)buf;
    (void)numSamples;
}

void SDRBackend_processAgc(void *cbContext, softbit_complex_t *buf, unsigned int numSamples) {
    (void)cbContext;
    (void)buf;
    (void)numSamples;
}

void SDRBackend_processPeak(void *cbContext, float *buf, unsigned int numSamples) {
    (void)cbContext;
    (void)buf;
    (void)numSamples;
}

void SDRBackend_processSNR(void *cbContext, float snr) {
    (void)cbContext;
    (void)snr;
}

void SDRBackend_processCFO(void *cbContext, float cfo) {
    (void)cbContext;
    (void)cfo;
    printf("CFO: %f\n", cfo);
}

void SDRBackend_processBER(void *cbContext, float BER) {
    (void)cbContext;
    (void)BER;
}

void SDRBackend_processMER(void *cbContext, float MER) {
    (void)cbContext;
    (void)MER;
}

void SDRBackend_processEVM(void *cbContext, float EVM) {
    (void)cbContext;
    (void)EVM;
}

void SDRBackend_processPAPR(void *cbContext, float PAPR) {
    (void)cbContext;
    (void)PAPR;
}

void SDRBackend_processRSSI(void *cbContext, float rssi) {
    (void)cbContext;
    (void)rssi;
}

void SDRBackend_processPAD(void *cbContext,
                           unsigned char *xpad_data,
                           unsigned long xpad_len,
                           unsigned char exact_xpad_len,
                           unsigned char *fpad_data) {
    (void)cbContext;
    (void)xpad_data;
    (void)xpad_len;
    (void)exact_xpad_len;
    (void)fpad_data;
}

void SDRBackend_processAudio(void *cbContext, char *data, unsigned long len) {
    (void)cbContext;
    (void)data;
    (void)len;
}
