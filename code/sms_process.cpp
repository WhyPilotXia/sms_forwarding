#include "sms_process.h"
#include "web_handlers.h"
#include "modem.h"
#include "web_handlers.h"
#include "push.h"
#include "web_handlers.h"
#include "wifi_manager.h"

// 初始化长短信缓存
void initConcatBuffer() {
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    concatBuffer[i].inUse = false;
    concatBuffer[i].receivedParts = 0;
    for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
      concatBuffer[i].parts[j].valid = false;
      concatBuffer[i].parts[j].text = "";
    }
  }
}

// 查找或创建长短信缓存槽位
int findOrCreateConcatSlot(int refNumber, const char* sender, int totalParts) {
  // 先查找是否已存在
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse && 
        concatBuffer[i].refNumber == refNumber &&
        concatBuffer[i].sender.equals(sender)) {
      return i;
    }
  }
  
  // 查找空闲槽位
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (!concatBuffer[i].inUse) {
      concatBuffer[i].inUse = true;
      concatBuffer[i].refNumber = refNumber;
      concatBuffer[i].sender = String(sender);
      concatBuffer[i].totalParts = totalParts;
      concatBuffer[i].receivedParts = 0;
      concatBuffer[i].firstPartTime = millis();
      for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
        concatBuffer[i].parts[j].valid = false;
        concatBuffer[i].parts[j].text = "";
      }
      return i;
    }
  }
  
  // 没有空闲槽位，查找最老的槽位覆盖
  int oldestSlot = 0;
  unsigned long oldestTime = concatBuffer[0].firstPartTime;
  for (int i = 1; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].firstPartTime < oldestTime) {
      oldestTime = concatBuffer[i].firstPartTime;
      oldestSlot = i;
    }
  }
  
  // 覆盖最老的槽位
  logCaptureLn(String("⚠️ 长短信缓存已满，覆盖最老的槽位"));
  concatBuffer[oldestSlot].inUse = true;
  concatBuffer[oldestSlot].refNumber = refNumber;
  concatBuffer[oldestSlot].sender = String(sender);
  concatBuffer[oldestSlot].totalParts = totalParts;
  concatBuffer[oldestSlot].receivedParts = 0;
  concatBuffer[oldestSlot].firstPartTime = millis();
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[oldestSlot].parts[j].valid = false;
    concatBuffer[oldestSlot].parts[j].text = "";
  }
  return oldestSlot;
}

// 合并长短信各分段
String assembleConcatSms(int slot) {
  String result = "";
  for (int i = 0; i < concatBuffer[slot].totalParts; i++) {
    if (concatBuffer[slot].parts[i].valid) {
      result += concatBuffer[slot].parts[i].text;
    } else {
      result += "[缺失分段" + String(i + 1) + "]";
    }
  }
  return result;
}

// 清空长短信槽位
void clearConcatSlot(int slot) {
  concatBuffer[slot].inUse = false;
  concatBuffer[slot].receivedParts = 0;
  concatBuffer[slot].sender = "";
  concatBuffer[slot].timestamp = "";
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[slot].parts[j].valid = false;
    concatBuffer[slot].parts[j].text = "";
  }
}

// 检查长短信超时并转发
void checkConcatTimeout() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse) {
      if (now - concatBuffer[i].firstPartTime >= CONCAT_TIMEOUT_MS) {
        logCaptureLn(String("⏰ 长短信超时，强制转发不完整消息"));
        logCaptureF("  参考号: %d, 已收到: %d/%d\n", 
                      concatBuffer[i].refNumber,
                      concatBuffer[i].receivedParts,
                      concatBuffer[i].totalParts);
        
        // 合并已收到的分段
        String fullText = assembleConcatSms(i);
        
        // 处理短信内容
        processSmsContent(concatBuffer[i].sender.c_str(), 
                         fullText.c_str(), 
                         concatBuffer[i].timestamp.c_str());
        
        // 清空槽位
        clearConcatSlot(i);
      }
    }
  }
}

// 读取串口一行（含回车换行），返回行字符串，无新行时返回空
String readSerialLine(HardwareSerial& port) {
  static char lineBuf[SERIAL_BUFFER_SIZE];
  static int linePos = 0;

  while (port.available()) {
    char c = port.read();
    if (c == '\n') {
      lineBuf[linePos] = 0;
      String res = String(lineBuf);
      linePos = 0;
      return res;
    } else if (c != '\r') {  // 跳过\r
      if (linePos < SERIAL_BUFFER_SIZE - 1)
        lineBuf[linePos++] = c;
      else
        linePos = 0;  //超长报错保护，重头计
    }
  }
  return "";
}

// 检查字符串是否为有效的十六进制PDU数据
bool isHexString(const String& str) {
  // PDU 每个字节必须由两个十六进制字符表示
  if (str.length() < 2 || (str.length() % 2) != 0) return false;
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

// CNMA 等待期间若又收到 URC，先保存在这里，随后仍由 checkSerial1URC() 统一处理。
// 这样 ACK 事务不会吞掉紧随其后的另一条短信。
#define DEFERRED_MODEM_LINE_COUNT 8
static String deferredModemLines[DEFERRED_MODEM_LINE_COUNT];
static uint8_t deferredModemHead = 0;
static uint8_t deferredModemTail = 0;
static uint8_t deferredModemCount = 0;

static bool deferModemLine(const String& line) {
  if (deferredModemCount >= DEFERRED_MODEM_LINE_COUNT) {
    logCaptureLn(String("⚠️ 模组延迟行缓冲区已满，丢弃: " + line));
    return false;
  }
  deferredModemLines[deferredModemTail] = line;
  deferredModemTail = (uint8_t)((deferredModemTail + 1) % DEFERRED_MODEM_LINE_COUNT);
  deferredModemCount++;
  return true;
}

// 对 +CMT 直推短信发送 RP-ACK。不能复用 sendATCommand()，它会先清空串口。
// 使用统一的逐行读取器，并把非本事务响应重新排队，避免吞掉并发 URC。
static bool acknowledgeDirectSms() {
  Serial1.println("AT+CNMA=1");

  unsigned long start = millis();
  while (millis() - start < SMS_ACK_TIMEOUT_MS) {
    String line = readSerialLine(Serial1);
    if (line.length() == 0) {
      delay(1);
      continue;
    }

    String trimmed = line;
    trimmed.trim();
    if (trimmed.length() == 0 || trimmed == "AT+CNMA=1") continue;
    if (trimmed == "OK") {
      logCaptureLn(String("✅ 短信接收确认成功 (AT+CNMA=1)"));
      return true;
    }
    if (trimmed == "ERROR" || trimmed.startsWith("+CME ERROR") ||
        trimmed.startsWith("+CMS ERROR")) {
      logCaptureLn(String("⚠️ 短信接收确认失败: " + trimmed));
      return false;
    }

    deferModemLine(line);
  }

  logCaptureLn(String("⚠️ 短信接收确认超时"));
  return false;
}

struct SmsPduFingerprint {
  uint64_t hash;
  uint16_t byteLength;
  unsigned long seenAt;
  bool valid;
};

static SmsPduFingerprint smsDedupCache[SMS_DEDUP_CACHE_SIZE] = {};
static uint8_t smsDedupNext = 0;

static uint8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
  if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
  return (uint8_t)(c - 'a' + 10);
}

// 对规范化后的 PDU 字节计算 FNV-1a 64 位指纹，避免十六进制大小写造成差异。
static uint64_t fingerprintPdu(const String& pduLine) {
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned int i = 0; i < pduLine.length(); i += 2) {
    uint8_t value = (uint8_t)((hexNibble(pduLine.charAt(i)) << 4) |
                              hexNibble(pduLine.charAt(i + 1)));
    hash ^= value;
    hash *= 1099511628211ULL;
  }
  return hash;
}

// 返回 true 表示该完整 PDU 在去重窗口内已经处理过；首次出现时写入缓存。
static bool isDuplicateSmsPdu(const String& pduLine, unsigned long* ageMs) {
  const uint64_t hash = fingerprintPdu(pduLine);
  const uint16_t byteLength = (uint16_t)(pduLine.length() / 2);
  const unsigned long now = millis();

  for (int i = 0; i < SMS_DEDUP_CACHE_SIZE; i++) {
    if (!smsDedupCache[i].valid || smsDedupCache[i].hash != hash ||
        smsDedupCache[i].byteLength != byteLength) {
      continue;
    }

    unsigned long age = now - smsDedupCache[i].seenAt; // 无符号减法可安全处理 millis 回绕
    if (age < SMS_DEDUP_WINDOW_MS) {
      if (ageMs != nullptr) *ageMs = age;
      return true;
    }

    // 相同指纹已过期，原位刷新，避免一个 PDU 占用多个环形槽位。
    smsDedupCache[i].seenAt = now;
    if (ageMs != nullptr) *ageMs = 0;
    return false;
  }

  smsDedupCache[smsDedupNext].hash = hash;
  smsDedupCache[smsDedupNext].byteLength = byteLength;
  smsDedupCache[smsDedupNext].seenAt = now;
  smsDedupCache[smsDedupNext].valid = true;
  smsDedupNext = (uint8_t)((smsDedupNext + 1) % SMS_DEDUP_CACHE_SIZE);
  if (ageMs != nullptr) *ageMs = 0;
  return false;
}

// 检查发送者是否在号码黑名单中
bool isInNumberBlackList(const char* sender) {
  if (config.numberBlackList.length() == 0) return false;

  String originalSender = String(sender);
  bool has86 = originalSender.startsWith("+86");
  String strippedSender = has86 ? originalSender.substring(3) : "";

  int listLen = (int)config.numberBlackList.length();

  int start = 0;
  while (start <= listLen) {
    int end = config.numberBlackList.indexOf('\n', start);
    if (end == -1) end = listLen;

    String line = config.numberBlackList.substring(start, end);
    line.trim();

    if (line.length() > 0 && (line.equals(originalSender) || (has86 && line.equals(strippedSender)))) {
      return true;
    }

    start = end + 1;
  }

  return false;
}

// 检查发送者是否为管理员
bool isAdmin(const char* sender) {
  if (config.adminPhone.length() == 0) return false;
  
  // 去除可能的国际区号前缀进行比较
  String senderStr = String(sender);
  String adminStr = config.adminPhone;
  
  // 去除+86前缀
  if (senderStr.startsWith("+86")) {
    senderStr = senderStr.substring(3);
  }
  if (adminStr.startsWith("+86")) {
    adminStr = adminStr.substring(3);
  }
  
  return senderStr.equals(adminStr);
}

// 处理管理员命令
void processAdminCommand(const char* sender, const char* text) {
  String cmd = String(text);
  cmd.trim();
  
  logCaptureLn(String("处理管理员命令: " + cmd));
  
  // 处理 SMS:号码:内容 命令
  if (cmd.startsWith("SMS:")) {
    int firstColon = cmd.indexOf(':');
    int secondColon = cmd.indexOf(':', firstColon + 1);
    
    if (secondColon > firstColon + 1) {
      String targetPhone = cmd.substring(firstColon + 1, secondColon);
      String smsContent = cmd.substring(secondColon + 1);
      
      targetPhone.trim();
      smsContent.trim();
      
      logCaptureLn(String("目标号码: " + targetPhone));
      logCaptureLn(String("短信内容: " + smsContent));
      
      bool success = sendSMS(targetPhone.c_str(), smsContent.c_str());
      
      // 发送邮件通知结果
      String subject = success ? "短信发送成功" : "短信发送失败";
      String body = "管理员命令执行结果:\n";
      body += "命令: " + cmd + "\n";
      body += "目标号码: " + targetPhone + "\n";
      body += "短信内容: " + smsContent + "\n";
      body += "执行结果: " + String(success ? "成功" : "失败");
      
      sendEmailNotification(subject.c_str(), body.c_str());
    } else {
      logCaptureLn(String("SMS命令格式错误"));
      sendEmailNotification("命令执行失败", "SMS命令格式错误，正确格式: SMS:号码:内容");
    }
  }
  // 处理 RESET 命令
  else if (cmd.equals("RESET")) {
    logCaptureLn(String("执行RESET命令"));
    
    // 先发送邮件通知（因为重启后就发不了了）
    sendEmailNotification("重启命令已执行", "收到RESET命令，即将重启模组和ESP32...");
    
    // 重启模组
    resetModule();
    
    // 重启ESP32
    logCaptureLn(String("正在重启ESP32..."));
    delay(1000);
    ESP.restart();
  }
  else {
    logCaptureLn(String("未知命令: " + cmd));
  }
}

// 处理最终的短信内容（管理员命令检查和转发）
void processSmsContent(const char* sender, const char* text, const char* timestamp) {
  logCaptureLn(String("=== 处理短信内容 ==="));
  logCaptureLn(String("发送者: " + String(sender)));
  logCaptureLn(String("时间戳: " + String(timestamp)));
  logCaptureLn(String("内容: " + String(text)));
  logCaptureLn(String("===================="));

  // 检查是否在号码黑名单中
  if (isInNumberBlackList(sender)) {
    logCaptureLn(String("发送者在号码黑名单中，忽略该短信"));
    return;
  }

  // 检查是否为管理员命令
  if (isAdmin(sender)) {
    logCaptureLn(String("收到管理员短信，检查命令..."));
    String smsText = String(text);
    smsText.trim();
    
    // 检查是否为命令格式
    if (smsText.startsWith("SMS:") || smsText.equals("RESET")) {
      processAdminCommand(sender, text);
      // 命令已处理，不再发送普通通知邮件
      return;
    }
  }

  // 发送通知http（推送到所有启用的通道）
  sendSMSToServer(sender, text, timestamp);
  // 发送通知邮件
  String subject = ""; subject+="短信";subject+=sender;subject+=",";subject+=text;
  String body = ""; body+="来自：";body+=sender;body+="，时间：";body+=timestamp;body+="，内容：";body+=text;
  body += "\n局域网设备地址：http://" + getActiveWifiAddress() + "/";
  sendEmailNotification(subject.c_str(), body.c_str());
}

// 处理URC和PDU
void checkSerial1URC() {
  static enum { IDLE,
                WAIT_PDU } state = IDLE;

  String line;
  if (deferredModemCount > 0) {
    line = deferredModemLines[deferredModemHead];
    deferredModemLines[deferredModemHead] = "";
    deferredModemHead = (uint8_t)((deferredModemHead + 1) % DEFERRED_MODEM_LINE_COUNT);
    deferredModemCount--;
  } else {
    line = readSerialLine(Serial1);
  }
  if (line.length() == 0) return;

  // 打印到调试串口
  logCaptureLn(String("Debug> " + line));

  if (state == IDLE) {
    // 检测到短信上报URC头
    if (line.startsWith("+CMT:")) {
      logCaptureLn(String("检测到+CMT，等待PDU数据..."));
      state = WAIT_PDU;
    }
  } else if (state == WAIT_PDU) {
    // 跳过空行
    if (line.length() == 0) {
      return;
    }
    
    // 如果是偶数长度的十六进制字符串，认为是PDU数据
    if (isHexString(line)) {
      // 已完整接收 PDU，先退出等待状态，再尽快确认网络侧投递。
      // 确认必须发生在 SMTP/HTTP 等耗时操作之前，否则网络可能重发同一条短信。
      state = IDLE;
      logCaptureLn(String("收到PDU数据: " + line));
      logCaptureLn(String("PDU长度: " + String(line.length()) + " 字符"));
      // 先做快速解析验证；成功后立即确认，再开始任何 SMTP/HTTP 耗时操作。
      // 解析失败时不发送正向确认，让网络侧保留重新投递机会。
      if (!pdu.decodePDU(line.c_str())) {
        logCaptureLn(String("❌ PDU解析失败，未发送AT+CNMA正向确认"));
      } else {
        acknowledgeDirectSms();

        // 第二道防线：即使模组或网络仍重发相同 PDU，也不重复通知。
        unsigned long duplicateAgeMs = 0;
        if (isDuplicateSmsPdu(line, &duplicateAgeMs)) {
          logCaptureF("♻️ 检测到重复短信PDU，已确认但跳过通知（距首次处理 %lu 秒）\n",
                      duplicateAgeMs / 1000UL);
          return;
        }

        logCaptureLn(String("✓ PDU解析成功"));
        logCaptureLn(String("=== 短信内容 ==="));
        logCaptureLn(String("发送者: " + String(pdu.getSender())));
        logCaptureLn(String("时间戳: " + String(pdu.getTimeStamp())));
        logCaptureLn(String("内容: " + String(pdu.getText())));
        
        // 获取长短信信息
        int* concatInfo = pdu.getConcatInfo();
        int refNumber = concatInfo[0];
        int partNumber = concatInfo[1];
        int totalParts = concatInfo[2];
        
        logCaptureF("长短信信息: 参考号=%d, 当前=%d, 总计=%d\n", refNumber, partNumber, totalParts);
        logCaptureLn(String("==============="));

        // 判断是否为长短信
        if (totalParts > 1 && partNumber > 0) {
          // 这是长短信的一部分
          logCaptureF("📧 收到长短信分段 %d/%d\n", partNumber, totalParts);
          
          // 查找或创建缓存槽位
          int slot = findOrCreateConcatSlot(refNumber, pdu.getSender(), totalParts);
          
          // 存储该分段（partNumber从1开始，数组从0开始）
          int partIndex = partNumber - 1;
          if (partIndex >= 0 && partIndex < MAX_CONCAT_PARTS) {
            if (!concatBuffer[slot].parts[partIndex].valid) {
              concatBuffer[slot].parts[partIndex].valid = true;
              concatBuffer[slot].parts[partIndex].text = String(pdu.getText());
              concatBuffer[slot].receivedParts++;
              
              // 如果是第一个收到的分段，保存时间戳
              if (concatBuffer[slot].receivedParts == 1) {
                concatBuffer[slot].timestamp = String(pdu.getTimeStamp());
              }
              
              logCaptureF("  已缓存分段 %d，当前已收到 %d/%d\n", 
                           partNumber, 
                           concatBuffer[slot].receivedParts, 
                           totalParts);
            } else {
              logCaptureF("  ⚠️ 分段 %d 已存在，跳过\n", partNumber);
            }
          }
          
          // 检查是否已收齐所有分段
          if (concatBuffer[slot].receivedParts >= totalParts) {
            logCaptureLn(String("✅ 长短信已收齐，开始合并转发"));
            
            // 合并所有分段
            String fullText = assembleConcatSms(slot);
            
            // 处理完整短信
            processSmsContent(concatBuffer[slot].sender.c_str(), 
                             fullText.c_str(), 
                             concatBuffer[slot].timestamp.c_str());
            
            // 清空槽位
            clearConcatSlot(slot);
          }
        } else {
          // 普通短信，直接处理
          processSmsContent(pdu.getSender(), pdu.getText(), pdu.getTimeStamp());
        }
      }
      
      // state 已在接收完整 PDU 后立即恢复为 IDLE
    } 
    // 如果是其他内容（OK、ERROR等），也返回IDLE
    else {
      logCaptureLn(String("收到非PDU数据，返回IDLE状态"));
      state = IDLE;
    }
  }
}
