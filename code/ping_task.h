#ifndef PING_TASK_H
#define PING_TASK_H

#include "globals.h"

enum PingTaskState {
  PING_IDLE = 0,
  PING_ACTIVATING,
  PING_WAIT_RESULT,
  PING_DEACTIVATING,
  PING_DONE
};

bool startPingTask();
void updatePingTask();
bool isPingTaskBusy();
String getPingTaskJson();

#endif
