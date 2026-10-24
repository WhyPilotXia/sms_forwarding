#ifndef NOTIFY_QUEUE_H
#define NOTIFY_QUEUE_H

#include "globals.h"

bool enqueueSmsNotifications(const char* sender, const char* message, const char* timestamp,
                             const char* subject, const char* body);
void processNotifyQueue();
bool notifyQueueBusy();

#endif
