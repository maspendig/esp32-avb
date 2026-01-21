//
// Created by max on 1/21/26.
//

#ifndef ETHERNET_PTP_AUDIO_H
#define ETHERNET_PTP_AUDIO_H
#include <sys/types.h>

void CODEC_INIT();
void CODEC_I2S_WRITE(const void* data, size_t size, size_t* bytes_written);
#endif //ETHERNET_PTP_AUDIO_H
