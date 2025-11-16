#include <esp_log.h>
#include "acmp.h"
#include "avtp.h"

#define TAG "acmp"

void send_acmp_message()
{
  ESP_LOGI(TAG, "Sending ACMP message");
  struct acmp_du_s msg = {0};
  msg.header.subtype = AVTP_SUBTYPE_ACMP;
  msg.header.h = 0;
  msg.header.version = 0;
  msg.header.message_type = ACMP_MSG_TYPE_CONNECT_TX_COMMAND;
  msg.header.status = 0; // SUCCESS
  msg.header.control_data_length = 44; // Size of ACMP payload after header
}
