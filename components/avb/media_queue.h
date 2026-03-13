//
// Created by max on 1/27/26.
//

#ifndef ETHERNET_PTP_MEDIA_QUEUE_H
#define ETHERNET_PTP_MEDIA_QUEUE_H

#include <stddef.h>
#include <stdbool.h>
#include "types.h"
#include "audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"

/* Maximum audio samples per packet (6 samples * max 8 channels) */
#define MEDIA_QUEUE_MAX_SAMPLES_PER_PACKET (6 * OUTPUT_CHANNELS)

/* Queue depth - number of packets that can be buffered */
#define MEDIA_QUEUE_DEPTH 256

/* Media sample entry containing audio data and timing info */
typedef struct media_queue_entry
{
  u32 avtp_timestamp; /* AVTP presentation timestamp */
#if SAMPLE_BIT_RATE == 16
  s16 samples[MEDIA_QUEUE_MAX_SAMPLES_PER_PACKET];
#else
  s32 samples[MEDIA_QUEUE_MAX_SAMPLES_PER_PACKET];
#endif
} media_queue_entry_t;

/* Media queue context */
typedef struct media_queue
{
  QueueHandle_t queue;
  TaskHandle_t consumer_task;
  bool running;

  /* Statistics */
  volatile u32 stats_q_drop;
  volatile u32 stats_seq_err;
  volatile u32 stats_received;
  volatile u32 stats_played;
} media_queue_t;

/**
 * @brief Initialize the media queue
 *
 * Creates the FreeRTOS queue for audio sample buffering.
 *
 * @param mq Pointer to media queue context
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t media_queue_init(media_queue_t* mq);

/**
 * @brief Start the media queue consumer task
 *
 * Creates a task that consumes audio samples from the queue
 * and writes them to the audio codec.
 *
 * @param mq Pointer to media queue context
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t media_queue_start(media_queue_t* mq);

/**
 * @brief Stop the media queue consumer task
 *
 * Signals the consumer task to stop and waits for it to exit.
 *
 * @param mq Pointer to media queue context
 */
void media_queue_stop(media_queue_t* mq);

/**
 * @brief Push audio samples to the media queue
 *
 * Non-blocking push of audio samples to the queue.
 * If the queue is full, the packet will be dropped.
 *
 * @param mq Pointer to media queue context
 * @param entry Pointer to the media queue entry to push
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if queue is full
 */
esp_err_t media_queue_push(media_queue_t* mq, const media_queue_entry_t* entry);

/**
 * @brief Deinitialize the media queue
 *
 * Frees all resources associated with the media queue.
 *
 * @param mq Pointer to media queue context
 */
void media_queue_deinit(media_queue_t* mq);

/**
 * @brief Flush all entries from the media queue
 *
 * Removes all pending entries from the queue.
 *
 * @param mq Pointer to media queue context
 */
void media_queue_flush(media_queue_t* mq);

#endif //ETHERNET_PTP_MEDIA_QUEUE_H

