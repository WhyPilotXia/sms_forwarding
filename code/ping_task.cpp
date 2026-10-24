#include "ping_task.h"
#include "web_handlers.h"
#include "push.h"

static PingTaskState pingState = PING_IDLE;
static unsigned long pingDeadline = 0;
static String pingResponse;
static String pingMessage;
static bool pingSuccess = false;

static void beginCommand(const char* command, unsigned long timeoutMs) {
  while (Serial1.available()) Serial1.read();
  pingResponse = "";
  Serial1.println(command);
  pingDeadline = millis() + timeoutMs;
}

static bool deadlineReached() {
  return (long)(millis() - pingDeadline) >= 0;
}

static bool responseHasFinal() {
  return pingResponse.indexOf("OK") >= 0 || pingResponse.indexOf("ERROR") >= 0 ||
         pingResponse.indexOf("+CME ERROR") >= 0 || pingResponse.indexOf("+CMS ERROR") >= 0;
}

static void finishPing() {
  pingState = PING_DONE;
  logCaptureLn(String("Ping 任务完成: " + pingMessage));
}

bool startPingTask() {
  if (pingState != PING_IDLE && pingState != PING_DONE) return false;
  pingSuccess = false;
  pingMessage = "正在激活数据连接";
  pingState = PING_ACTIVATING;
  logCaptureLn(String("网页端启动非阻塞 Ping 任务"));
  beginCommand("AT+CGACT=1,1", 10000);
  return true;
}

void updatePingTask() {
  if (pingState == PING_IDLE || pingState == PING_DONE) return;

  while (Serial1.available()) {
    pingResponse += (char)Serial1.read();
  }

  if (pingState == PING_ACTIVATING) {
    if (responseHasFinal() || deadlineReached()) {
      logCaptureLn(String("CGACT激活响应: " + pingResponse));
      pingMessage = "正在 Ping 8.8.8.8";
      pingState = PING_WAIT_RESULT;
      beginCommand("AT+MPING=\"8.8.8.8\",15,1", 20000);
    }
    return;
  }

  if (pingState == PING_WAIT_RESULT) {
    int idx = pingResponse.indexOf("+MPING:");
    int end = idx >= 0 ? pingResponse.indexOf('\n', idx) : -1;
    if (idx >= 0 && end >= 0) {
      String line = pingResponse.substring(idx, end);
      line.trim();
      logCaptureLn(String("收到MPING结果: " + line));
      String params = line.substring(line.indexOf(':') + 1);
      params.trim();
      int result = params.toInt();
      pingSuccess = (result == 0 || result == 1) && params.indexOf(',') >= 0;
      pingMessage = pingSuccess ? ("Ping 成功: " + line) : ("Ping 失败: " + line);
      pingState = PING_DEACTIVATING;
      beginCommand("AT+CGACT=0,1", 5000);
    } else if (deadlineReached() || pingResponse.indexOf("ERROR") >= 0) {
      pingSuccess = false;
      pingMessage = deadlineReached() ? "Ping 超时" : "模组返回错误";
      pingState = PING_DEACTIVATING;
      beginCommand("AT+CGACT=0,1", 5000);
    }
    return;
  }

  if (pingState == PING_DEACTIVATING && (responseHasFinal() || deadlineReached())) {
    logCaptureLn(String("CGACT关闭响应: " + pingResponse));
    finishPing();
  }
}

bool isPingTaskBusy() {
  return pingState != PING_IDLE && pingState != PING_DONE;
}

String getPingTaskJson() {
  String json = "{";
  json += "\"running\":" + String(isPingTaskBusy() ? "true" : "false") + ",";
  json += "\"done\":" + String(pingState == PING_DONE ? "true" : "false") + ",";
  json += "\"success\":" + String(pingSuccess ? "true" : "false") + ",";
  json += "\"message\":\"" + jsonEscape(pingMessage) + "\"}";
  return json;
}
