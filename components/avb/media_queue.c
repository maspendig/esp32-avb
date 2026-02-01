//
// Created by max on 1/27/26.
//

#include "media_queue.h"

#include <esp_private/systimer.h>
#include <hal/systimer_hal.h>

#include "audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_eth_time.h"
#include "esp_attr.h"

static const char* TAG = "m_queue";

static u64 get_ptptime(void)
{
  struct timespec ts;
  if (esp_eth_clock_gettime(CLOCK_PTP_SYSTEM, &ts) == 0)
  {
    return ((u64)ts.tv_sec * 1000000000ULL) + ts.tv_nsec;
  }
  return 0;
}

u32 timespec_to_avtp_timestamp(struct timespec gptp_time)
{
  // avtp_timestamp = (AS_sec * 10^9 + AS_nsec) mod 2^32
  return (u32)(((u64)gptp_time.tv_sec * 1000000000ULL + gptp_time.tv_nsec) & 0xFFFFFFFF);
}

u32 u64_to_avtp_timestamp(u64 gptp_time)
{
  // avtp_timestamp = (AS_sec * 10^9 + AS_nsec) mod 2^32
  return (u32)(gptp_time & 0xFFFFFFFF);
}

/* Consumer task configuration */
#define MEDIA_QUEUE_CONSUMER_TASK_STACK  4096
#define MEDIA_QUEUE_CONSUMER_TASK_PRIO   18
#define MEDIA_QUEUE_CONSUMER_TASK_CORE   0

u64 IRAM_ATTR ts_to_playback_ns(u32* ts, u64* now_ns)
{
  u32 now_ts = u64_to_avtp_timestamp(*now_ns);
  u32 delta;
  u64 playback_time_ns;
  if (now_ts < *ts)
  {
    delta = *ts - now_ts;
  }
  else if (now_ts > *ts)
  {
    delta = *ts + (0x100000000ULL - now_ts);
  }
  else
  {
    delta = 0;
  }
  if (delta < 0x7FFFFFFF)
  {
    playback_time_ns = *now_ns + delta;
  }
  else
  {
    playback_time_ns = *now_ns - (0x100000000ULL - delta);
  }
  return playback_time_ns;
}

/**
 * @brief Consumer task that reads from the queue and writes to codec
 *
 * Currently writes immediately to the codec. Future enhancement will
 * use avtp_timestamp and gPTP time for synchronized playback.
 */
static IRAM_ATTR void media_queue_consumer_task(void* arg)
{
  media_queue_t* mq = (media_queue_t*)arg;
  media_queue_entry_t entry;

  ESP_LOGI(TAG, "Media Queue task started on core %d (priority %d)",
           xPortGetCoreID(), uxTaskPriorityGet(NULL));

  u64 last_log_time_ns = 0;
  size_t drops = 0;
  bool init = true;
  while (mq->running)
  {
    /* Wait for an entry with a short timeout to allow checking running flag */
    if (xQueueReceive(mq->queue, &entry, pdMS_TO_TICKS(10)) == pdTRUE)
    {
      if (entry.avtp_timestamp != 0)
      {
        u64 now_ns = get_ptptime();
        u64 playback_time_ns = ts_to_playback_ns(&entry.avtp_timestamp, &now_ns);
        //
        //        if (init && ((playback_time_ns + 10000000) < now_ns || (playback_time_ns - now_ns) > 2000000000ULL))
        //        {
        //          drops++;
        //          continue;
        //        }
        //        init = false;
        //
        //        while (playback_time_ns - now_ns > 10000)
        //        {
        //          now_ns = get_ptptime();
        //        }
        //
        // print delta every 2 seconds only once
        if (now_ns - last_log_time_ns > 2000000000ULL)
        {
          last_log_time_ns = now_ns;
          ESP_LOGI(TAG, "Status: q_len=%d, played=%lu, recv=%lu, seq_err=%lu, q_drop=%lu, delta=%lld ns",
                   uxQueueMessagesWaiting(mq->queue),
                   mq->stats_played,
                   mq->stats_received,
                   mq->stats_seq_err,
                   mq->stats_q_drop,
                   (long long int)(playback_time_ns - now_ns)
          );
        }
      }
      // currently only AM824 with 6 samples per packet is supported
      size_t bytes_to_write = 6 * OUTPUT_CHANNELS * sizeof(entry.samples[0]);
      u32 bytes_written = 0;
      CODEC_I2S_WRITE(entry.samples, bytes_to_write, &bytes_written);
      mq->stats_played++;
    }
  }

  ESP_LOGI(TAG, "Media queue consumer task exiting");
  vTaskDelete(NULL);
}

esp_err_t media_queue_init(media_queue_t* mq)
{
  if (mq == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  mq->queue = xQueueCreate(MEDIA_QUEUE_DEPTH, sizeof(media_queue_entry_t));
  if (mq->queue == NULL)
  {
    ESP_LOGE(TAG, "Failed to create media queue");
    return ESP_ERR_NO_MEM;
  }

  mq->consumer_task = NULL;
  mq->running = false;
  mq->stats_q_drop = 0;
  mq->stats_seq_err = 0;
  mq->stats_received = 0;
  mq->stats_played = 0;

  ESP_LOGI(TAG, "Media queue initialized (depth=%d, entry_size=%zu bytes)",
           MEDIA_QUEUE_DEPTH, sizeof(media_queue_entry_t));

  return ESP_OK;
}

esp_err_t media_queue_start(media_queue_t* mq)
{
  if (mq == NULL || mq->queue == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  if (mq->running)
  {
    ESP_LOGW(TAG, "Media queue consumer already running");
    return ESP_OK;
  }

  mq->running = true;

  BaseType_t ret = xTaskCreatePinnedToCore(
    media_queue_consumer_task,
    "mq_consumer",
    MEDIA_QUEUE_CONSUMER_TASK_STACK,
    mq,
    MEDIA_QUEUE_CONSUMER_TASK_PRIO,
    &mq->consumer_task,
    MEDIA_QUEUE_CONSUMER_TASK_CORE
  );

  if (ret != pdPASS)
  {
    ESP_LOGE(TAG, "Failed to create media queue consumer task");
    mq->running = false;
    return ESP_FAIL;
  }

  return ESP_OK;
}

void media_queue_stop(media_queue_t* mq)
{
  ESP_LOGI(TAG, "Stopping media queue consumer task");
  if (mq == NULL)
  {
    return;
  }

  mq->running = false;

  /* Give the task time to exit gracefully */
  if (mq->consumer_task != NULL)
  {
    vTaskDelay(pdMS_TO_TICKS(50));
    mq->consumer_task = NULL;
  }

  /* Flush any remaining entries */
  media_queue_flush(mq);
}

esp_err_t IRAM_ATTR media_queue_push(media_queue_t* mq, const media_queue_entry_t* entry)
{
  if (mq == NULL || mq->queue == NULL || entry == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  mq->stats_received++;

  /* Non-blocking push - if queue is full, packet is dropped */
  if (xQueueSendToBack(mq->queue, entry, 0) != pdTRUE)
  {
    /* Queue full - this is expected under heavy load */
    mq->stats_q_drop++;
    return ESP_ERR_TIMEOUT;
  }

  return ESP_OK;
}

void media_queue_deinit(media_queue_t* mq)
{
  if (mq == NULL)
  {
    return;
  }

  /* Stop consumer task first */
  media_queue_stop(mq);

  if (mq->queue != NULL)
  {
    vQueueDelete(mq->queue);
    mq->queue = NULL;
  }

  ESP_LOGI(TAG, "Media queue deinitialized");
}

void media_queue_flush(media_queue_t* mq)
{
  if (mq == NULL || mq->queue == NULL)
  {
    return;
  }

  xQueueReset(mq->queue);
  ESP_LOGV(TAG, "Media queue flushed");
}
