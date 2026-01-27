//
// Created by max on 1/27/26.
//

#include "media_queue.h"
#include "audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "m_queue";

/* Consumer task configuration */
#define MEDIA_QUEUE_CONSUMER_TASK_STACK  4096
#define MEDIA_QUEUE_CONSUMER_TASK_PRIO   16
#define MEDIA_QUEUE_CONSUMER_TASK_CORE   1   /* Same core as stream processing */

/**
 * @brief Consumer task that reads from the queue and writes to codec
 *
 * Currently writes immediately to the codec. Future enhancement will
 * use avtp_timestamp and gPTP time for synchronized playback.
 */
static void media_queue_consumer_task(void* arg)
{
  media_queue_t* mq = (media_queue_t*)arg;
  media_queue_entry_t entry;

  ESP_LOGI(TAG, "Media Queue task started on core %d (priority %d)",
           xPortGetCoreID(), uxTaskPriorityGet(NULL));

  while (mq->running)
  {
    /* Wait for an entry with a short timeout to allow checking running flag */
    if (xQueueReceive(mq->queue, &entry, pdMS_TO_TICKS(10)) == pdTRUE)
    {
      /* TODO: In the future, check presentation time against gPTP clock
       * and wait until the appropriate time to play the samples.
       * For now, write immediately to the codec.
       */
      if (entry.sample_count > 0)
      {
        size_t bytes_to_write = entry.sample_count * OUTPUT_CHANNELS * sizeof(entry.samples[0]);
        u32 bytes_written = 0;
        CODEC_I2S_WRITE(entry.samples, bytes_to_write, &bytes_written);
      }
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

esp_err_t media_queue_push(media_queue_t* mq, const media_queue_entry_t* entry)
{
  if (mq == NULL || mq->queue == NULL || entry == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  /* Non-blocking push - if queue is full, packet is dropped */
  if (xQueueSend(mq->queue, entry, 0) != pdTRUE)
  {
    /* Queue full - this is expected under heavy load */
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
  ESP_LOGD(TAG, "Media queue flushed");
}
