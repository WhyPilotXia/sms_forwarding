#include "notify_queue.h"

#include "push.h"
#include "web_handlers.h"

namespace {
constexpr uint8_t NOTIFY_QUEUE_CAPACITY = 4;

struct NotifyJob {
  String sender;
  String message;
  String timestamp;
  String subject;
  String body;
};

NotifyJob notifyQueue[NOTIFY_QUEUE_CAPACITY];
uint8_t notifyQueueHead = 0;
uint8_t notifyQueueTail = 0;
uint8_t notifyQueueCount = 0;
SemaphoreHandle_t notifyQueueMutex = nullptr;
TaskHandle_t notifyTaskHandle = nullptr;

String ownedString(const char* value) {
  return value != nullptr ? String(value) : String();
}

void notifyTask(void*) {
  for (;;) {
    // Sleep until a new notification arrives; network work runs outside loop(),
    // so WebServer and Serial1 processing remain responsive.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    for (;;) {
      NotifyJob job;
      bool haveJob = false;
      if (xSemaphoreTake(notifyQueueMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (notifyQueueCount > 0) {
          job = notifyQueue[notifyQueueHead];
          notifyQueue[notifyQueueHead] = NotifyJob();
          notifyQueueHead = (notifyQueueHead + 1) % NOTIFY_QUEUE_CAPACITY;
          --notifyQueueCount;
          haveJob = true;
        }
        xSemaphoreGive(notifyQueueMutex);
      }

      if (!haveJob) break;

      logCaptureLn(String("📨 开始异步发送短信通知"));
      sendSMSToServer(job.sender.c_str(), job.message.c_str(), job.timestamp.c_str());
      sendEmailNotification(job.subject.c_str(), job.body.c_str());
      logCaptureLn(String("📨 异步短信通知处理完成"));
    }
  }
}

void ensureNotifyTask() {
  if (notifyQueueMutex == nullptr) {
    notifyQueueMutex = xSemaphoreCreateMutex();
  }
  if (notifyTaskHandle == nullptr && notifyQueueMutex != nullptr) {
    xTaskCreate(notifyTask, "sms_notify", 12288, nullptr, 1, &notifyTaskHandle);
  }
}
}  // namespace

bool enqueueSmsNotifications(const char* sender, const char* message, const char* timestamp,
                             const char* subject, const char* body) {
  ensureNotifyTask();
  if (notifyQueueMutex == nullptr || notifyTaskHandle == nullptr) {
    logCaptureLn(String("⚠️ 通知任务未启动，本条短信通知丢弃"));
    return false;
  }

  if (xSemaphoreTake(notifyQueueMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    logCaptureLn(String("⚠️ 通知队列忙，本条短信通知丢弃"));
    return false;
  }

  if (notifyQueueCount >= NOTIFY_QUEUE_CAPACITY) {
    xSemaphoreGive(notifyQueueMutex);
    logCaptureLn(String("⚠️ 通知队列已满，本条短信通知丢弃"));
    return false;
  }

  NotifyJob& job = notifyQueue[notifyQueueTail];
  job.sender = ownedString(sender);
  job.message = ownedString(message);
  job.timestamp = ownedString(timestamp);
  job.subject = ownedString(subject);
  job.body = ownedString(body);
  notifyQueueTail = (notifyQueueTail + 1) % NOTIFY_QUEUE_CAPACITY;
  ++notifyQueueCount;
  xSemaphoreGive(notifyQueueMutex);

  xTaskNotifyGive(notifyTaskHandle);
  return true;
}

// 保留该 API 兼容主循环；实际通知已由独立 FreeRTOS 任务处理。
void processNotifyQueue() {
}

bool notifyQueueBusy() {
  if (notifyQueueMutex == nullptr) return false;
  bool busy = false;
  if (xSemaphoreTake(notifyQueueMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    busy = notifyQueueCount > 0;
    xSemaphoreGive(notifyQueueMutex);
  }
  return busy;
}
