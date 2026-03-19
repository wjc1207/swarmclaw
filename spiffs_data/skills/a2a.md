# a2a_skill — 用 HTTP tool 实现 A2A 通信

## 概述

ESP32 AI agent 通过标准 HTTP tool 实现 A2A 协议通信，无需额外 SDK。
A2A 本质是 HTTP POST + JSON，所有交互通过两个固定格式完成。

---

## 作为 Client：主动发消息给远端 A2A agent

### 第一步：读取 Agent Card（可选，首次或缓存过期时）

```
HTTP GET {agent_url}/.well-known/agent-card.json
```

从返回结果确认对方支持的 skill，以及正确的 `url` 字段（即 POST endpoint）。

### 第二步：发送任务

```
HTTP POST {agent_card.url}
Content-Type: application/json

{
  "jsonrpc": "2.0",
  "id": "{任意唯一字符串}",
  "method": "message/send",
  "params": {
    "message": {
      "role": "user",
      "messageId": "{唯一id}",
      "contextId": "{会话id}",
      "parts": [
        { "text": "{你的请求内容}" }
      ]
    }
  }
}
```

### 第三步：解析返回

成功时从以下路径取结果文本：

```
response.result.artifacts[0].parts[0].text
```

失败时检查：

```
response.error.code
response.error.message
```

### 完整示例

```c
// 1. 构造 body
char body[512];
snprintf(body, sizeof(body),
    "{"
      "\"jsonrpc\":\"2.0\","
      "\"id\":\"req-001\","
      "\"method\":\"message/send\","
      "\"params\":{\"message\":{"
        "\"role\":\"user\","
        "\"messageId\":\"msg-001\","
        "\"contextId\":\"ctx-001\","
        "\"parts\":[{\"text\":\"%s\"}]"
      "}}"
    "}",
    request_text);

// 2. 发送
char reply[512];
http_post(AGENT_URL, body, reply, sizeof(reply));

// 3. 解析
cJSON *root     = cJSON_Parse(reply);
cJSON *result   = cJSON_GetObjectItem(root, "result");
cJSON *artifacts = cJSON_GetObjectItem(result, "artifacts");
cJSON *part     = cJSON_GetArrayItem(
                    cJSON_GetObjectItem(
                      cJSON_GetArrayItem(artifacts, 0), "parts"), 0);
const char *text = cJSON_GetStringValue(
                     cJSON_GetObjectItem(part, "text"));
```

---

## 作为 Server：响应远端 A2A agent 的调用

### 需要实现的两个 endpoint

#### GET `/.well-known/agent-card.json`

返回固定 JSON，描述本机能力：

```json
{
  "name": "ESP32-Agent",
  "description": "ESP32 sensor agent",
  "version": "1.0.0",
  "url": "http://{ESP32_IP}/message/send",
  "protocolVersion": "0.3.0",
  "defaultInputModes": ["text"],
  "defaultOutputModes": ["text"],
  "capabilities": { "streaming": false },
  "skills": [
    {
      "id": "read_temperature",
      "name": "Read Temperature",
      "description": "Returns current temperature in Celsius",
      "tags": ["sensor", "temperature"]
    }
  ]
}
```

#### POST `/message/send`

收到请求后：

1. 从 `params.message.parts[0].text` 取出请求文本
2. 匹配 skill，执行对应动作
3. 返回结果

**成功响应：**

```json
{
  "jsonrpc": "2.0",
  "id": "{原样返回请求的id}",
  "result": {
    "id": "{task_id}",
    "contextId": "{原样返回}",
    "status": { "state": "TASK_STATE_COMPLETED" },
    "artifacts": [
      {
        "artifactId": "{artifact_id}",
        "parts": [{ "text": "{结果内容}" }]
      }
    ]
  }
}
```

**失败响应：**

```json
{
  "jsonrpc": "2.0",
  "id": "{原样返回请求的id}",
  "error": {
    "code": -32603,
    "message": "Sensor read failed"
  }
}
```

### Skill handler 示例

```c
esp_err_t handle_message(httpd_req_t *req) {
    // 读 body
    char body[512];
    httpd_req_recv(req, body, sizeof(body));

    // 解析请求文本
    cJSON *root = cJSON_Parse(body);
    cJSON *id   = cJSON_GetObjectItem(root, "id");
    const char *text = cJSON_GetStringValue(
        cJSON_GetObjectItem(
            cJSON_GetArrayItem(
                cJSON_GetObjectItem(
                    cJSON_GetObjectItem(
                        cJSON_GetObjectItem(root, "params"),
                    "message"), "parts"), 0),
        "text"));

    // 匹配 skill
    char result[64];
    if (strstr(text, "temperature")) {
        snprintf(result, sizeof(result), "%.1f °C", read_temperature());
    } else if (strstr(text, "humidity")) {
        snprintf(result, sizeof(result), "%.1f %%RH", read_humidity());
    } else {
        // 未知请求
        send_error(req, id, -32601, "Unknown skill");
        cJSON_Delete(root);
        return ESP_OK;
    }

    // 构造响应
    send_result(req, id, result);
    cJSON_Delete(root);
    return ESP_OK;
}
```

---

## 错误码参考

| code | 含义 |
|---|---|
| `-32700` | JSON 解析失败 |
| `-32600` | 请求格式错误 |
| `-32601` | skill 不存在 |
| `-32603` | 内部错误（传感器失败等） |
| `-32001` | task 不存在 |

---

## 注意事项

- `id` 字段必须原样返回，调用方靠它匹配请求和响应
- `contextId` 代表一次会话，多轮对话保持同一个值
- Agent Card 的 `url` 字段必须是实际可 POST 的完整地址
- ESP32 作为 client 时不需要实现 Agent Card 和 server endpoint