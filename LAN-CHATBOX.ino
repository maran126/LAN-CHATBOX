#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <esp_partition.h>

// ─── CONFIG ───
const char* ssid     = "Maran's edge";
const char* password = "98765432";
const char* ADMIN_PASS = "admin123";
const char* MSG_FILE   = "/chat.json";
const int   MAX_FILE_SIZE = 50000;

// ⚠️ Set to "spiffs" if you flash the new partitions.csv (Option B),
//    or "ffat" if you want to try mounting on the existing partition (Option A).
#define FS_PARTITION_LABEL "spiffs"

WebServer server(80);
bool fsReady = false;

// ─── STRUCTS ───
struct User {
  String name; String ip; bool isAdmin; bool typing;
  String room; unsigned long lastSeen;
};
struct Message {
  String sender, text, to, room, file, fileType;
  unsigned long ts;
};
struct Activity {
  String user, action; unsigned long ts;
};

const int MAX_USERS      = 10;
const int MAX_MESSAGES   = 50;
const int MAX_ACTIVITIES = 20;
const int MAX_BANNED     = 10;

User     users[MAX_USERS];         int userCount = 0;
Message  messages[MAX_MESSAGES];   int msgCount = 0, msgIndex = 0;
Activity activities[MAX_ACTIVITIES]; int actCount = 0, actIndex = 0;
String   banned[MAX_BANNED];       int bannedCount = 0;

const char* ROOMS[] = {"General", "Tech", "Random"};
const int   ROOM_COUNT = 3;

// ─── LOGGING HELPERS ───
#define LOG(fmt, ...) Serial.printf("[%8lu] " fmt "\n", millis(), ##__VA_ARGS__)

void logRequest() {
  String ip   = server.client().remoteIP().toString();
  String meth = (server.method() == HTTP_GET) ? "GET" : "POST";
  LOG("[HTTP] %s %s  from %s", meth.c_str(), server.uri().c_str(), ip.c_str());
}

// ─── HELPERS ───
int findUserByName(String name) {
  for (int i = 0; i < userCount; i++) if (users[i].name == name) return i;
  return -1;
}
int findUserByIp(String ip) {
  for (int i = 0; i < userCount; i++) if (users[i].ip == ip) return i;
  return -1;
}
void updateIp(String name, String newIp) {
  int idx = findUserByName(name);
  if (idx >= 0) { users[idx].ip = newIp; users[idx].lastSeen = millis(); }
}
bool isBanned(String ip) {
  for (int i = 0; i < bannedCount; i++) if (banned[i] == ip) return true;
  return false;
}
bool adminExists() {
  for (int i = 0; i < userCount; i++) if (users[i].isAdmin) return true;
  return false;
}
void addActivity(String user, String action) {
  activities[actIndex].user   = user;
  activities[actIndex].action = action;
  activities[actIndex].ts     = millis();
  actIndex = (actIndex + 1) % MAX_ACTIVITIES;
  if (actCount < MAX_ACTIVITIES) actCount++;
  LOG("[ACT] %s %s", user.c_str(), action.c_str());
}

void saveMessages();
void addMessage(String sender, String text, String to, String room, String file, String fileType) {
  messages[msgIndex].sender   = sender;
  messages[msgIndex].text     = text;
  messages[msgIndex].to       = to;
  messages[msgIndex].room     = room;
  messages[msgIndex].file     = file;
  messages[msgIndex].fileType = fileType;
  messages[msgIndex].ts       = millis();
  msgIndex = (msgIndex + 1) % MAX_MESSAGES;
  if (msgCount < MAX_MESSAGES) msgCount++;
  LOG("[MSG] stored from='%s' to='%s' room='%s' file=%s len=%u",
      sender.c_str(), to.c_str(), room.c_str(),
      file.length() ? "yes" : "no", text.length());
  saveMessages();
}

// ─── FILESYSTEM ───
bool initFilesystem() {
  LOG("[FS] Looking for data partition label='%s'", FS_PARTITION_LABEL);

  const esp_partition_t* p = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, FS_PARTITION_LABEL);

  if (!p) {
    LOG("[FS] Partition '%s' NOT found. Available data partitions:", FS_PARTITION_LABEL);
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it) {
      const esp_partition_t* q = esp_partition_get(it);
      LOG("     label='%s'  subtype=0x%02X  offset=0x%X  size=0x%X",
          q->label, q->subtype, q->address, q->size);
      it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
    return false;
  }

  LOG("[FS] Mounting '%s' subtype=0x%02X @ 0x%X size=0x%X",
      p->label, p->subtype, p->address, p->size);

  if (!LittleFS.begin(true, "/littlefs", 10, FS_PARTITION_LABEL)) {
    LOG("[FS] LittleFS.begin() FAILED (subtype mismatch? try a 'spiffs' subtype)");
    return false;
  }

  LOG("[FS] OK  total=%u used=%u",
      (unsigned)LittleFS.totalBytes(), (unsigned)LittleFS.usedBytes());
  return true;
}

void saveMessages() {
  if (!fsReady) { LOG("[FS] save skipped (fsReady=false)"); return; }
  File f = LittleFS.open(MSG_FILE, "w");
  if (!f) { LOG("[FS] open-for-write FAILED: %s", MSG_FILE); return; }
  String json = "[";
  for (int i = 0; i < msgCount; i++) {
    int idx = (msgIndex - msgCount + i + MAX_MESSAGES) % MAX_MESSAGES;
    if (i > 0) json += ",";
    json += "{\"s\":\"" + messages[idx].sender + "\",\"t\":\"" + messages[idx].text +
            "\",\"to\":\"" + messages[idx].to + "\",\"r\":\"" + messages[idx].room +
            "\",\"f\":\"" + messages[idx].file + "\",\"ft\":\"" + messages[idx].fileType +
            "\",\"ts\":" + String(messages[idx].ts) + "}";
  }
  json += "]";
  size_t written = f.print(json);
  f.close();
  LOG("[FS] saved %u msgs (%u bytes) to %s", (unsigned)msgCount, (unsigned)written, MSG_FILE);
}

void loadMessages() {
  if (!fsReady) { LOG("[FS] load skipped (fsReady=false)"); return; }
  if (!LittleFS.exists(MSG_FILE)) { LOG("[FS] %s not found, starting empty", MSG_FILE); return; }
  File f = LittleFS.open(MSG_FILE, "r");
  if (!f) { LOG("[FS] open-for-read FAILED"); return; }
  String data = f.readString();
  f.close();
  LOG("[FS] loaded %u bytes from %s", (unsigned)data.length(), MSG_FILE);

  int pos = 0;
  msgCount = 0; msgIndex = 0;
  while ((pos = data.indexOf("{", pos)) != -1) {
    int end = data.indexOf("}", pos);
    if (end == -1) break;
    String obj = data.substring(pos, end + 1);
    pos = end + 1;
    String s  = obj.substring(obj.indexOf("\"s\":\"")+5,  obj.indexOf("\",\"t\""));
    String t  = obj.substring(obj.indexOf("\"t\":\"")+5,  obj.indexOf("\",\"to\""));
    String to = obj.substring(obj.indexOf("\"to\":\"")+6, obj.indexOf("\",\"r\""));
    String r  = obj.substring(obj.indexOf("\"r\":\"")+5,  obj.indexOf("\",\"f\""));
    String f_ = obj.substring(obj.indexOf("\"f\":\"")+5,  obj.indexOf("\",\"ft\""));
    String ft = obj.substring(obj.indexOf("\"ft\":\"")+6, obj.indexOf("\",\"ts\""));
    unsigned long ts = obj.substring(obj.indexOf("\"ts\":")+5).toInt();
    if (msgCount < MAX_MESSAGES) {
      messages[msgIndex] = {s, t, to, r, f_, ft, ts};
      msgIndex = (msgIndex + 1) % MAX_MESSAGES;
      msgCount++;
    }
  }
  LOG("[FS] parsed %d messages", msgCount);
}

// ─── WIFI ───
void checkWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG("[WIFI] lost, reconnecting (status=%d)", WiFi.status());
    WiFi.reconnect();
  }
}

// ─── HANDLERS ───
void handleLogin() {
  logRequest();
  String name = server.arg("name");
  String pass = server.arg("pass");
  String room = server.arg("room");
  String ip   = server.client().remoteIP().toString();

  LOG("[LOGIN] name='%s' passLen=%u room='%s' ip=%s users=%d",
      name.c_str(), (unsigned)pass.length(), room.c_str(), ip.c_str(), userCount);

  if (name.length() == 0) {
    LOG("[LOGIN] reject: empty name");
    server.send(400, "application/json", "{\"error\":\"Name required\"}");
    return;
  }
  if (name.length() > 20) name = name.substring(0, 20);
  if (isBanned(ip)) {
    LOG("[LOGIN] reject: IP is banned");
    server.send(403, "application/json", "{\"error\":\"You have been removed\"}");
    return;
  }
  if (room.length() == 0) room = "General";

  bool isAdmin = false;
  if (pass == ADMIN_PASS) {
    if (adminExists()) {
      LOG("[LOGIN] reject: admin slot taken");
      server.send(403, "application/json", "{\"error\":\"Admin slot taken\"}");
      return;
    }
    isAdmin = true;
    LOG("[LOGIN] granted admin");
  }

  for (int i = 0; i < userCount; i++) {
    if (users[i].name == name) {
      LOG("[LOGIN] duplicate name '%s' removed (was index %d)", name.c_str(), i);
      users[i] = users[userCount - 1];
      userCount--;
      i--;
    }
  }

  if (userCount < MAX_USERS) {
    users[userCount] = {name, ip, isAdmin, false, room, millis()};
    userCount++;
    LOG("[LOGIN] OK  name='%s' admin=%d users=%d", name.c_str(), isAdmin, userCount);
  } else {
    LOG("[LOGIN] reject: user table full");
    server.send(503, "application/json", "{\"error\":\"Server full\"}");
    return;
  }

  addActivity(name, "joined " + room);
  server.send(200, "application/json",
    "{\"ok\":true,\"name\":\"" + name + "\",\"admin\":" + (isAdmin ? "true" : "false") + "}");
}

void handleSend() {
  logRequest();
  String name = server.arg("name");
  String text = server.arg("text");
  String to   = server.arg("to");
  String room = server.arg("room");
  String ip   = server.client().remoteIP().toString();

  if (isBanned(ip)) { LOG("[SEND] reject: banned IP"); server.send(403, "text/plain", "Banned"); return; }
  int idx = findUserByName(name);
  if (idx < 0) { LOG("[SEND] reject: '%s' not logged in", name.c_str()); server.send(401, "text/plain", "Not logged in"); return; }

  updateIp(name, ip);
  users[idx].typing = false;
  if (room.length() > 0) users[idx].room = room;

  LOG("[SEND] from='%s' to='%s' room='%s' len=%u",
      name.c_str(), to.c_str(), room.c_str(), (unsigned)text.length());

  if (text.length() > 0) {
    if (text.length() > 300) text = text.substring(0, 300);
    addMessage(name, text, to, room, "", "");
  }
  server.send(200, "text/plain", "OK");
}

void handleSendFile() {
  logRequest();
  String name     = server.arg("name");
  String room     = server.arg("room");
  String file     = server.arg("file");
  String fileType = server.arg("fType");
  String ip       = server.client().remoteIP().toString();

  if (isBanned(ip)) { LOG("[FILE] reject: banned"); server.send(403, "text/plain", "Banned"); return; }
  int idx = findUserByName(name);
  if (idx < 0) { LOG("[FILE] reject: '%s' not logged in", name.c_str()); server.send(401, "text/plain", "Not logged in"); return; }

  updateIp(name, ip);
  LOG("[FILE] from='%s' room='%s' type='%s' b64len=%u",
      name.c_str(), room.c_str(), fileType.c_str(), (unsigned)file.length());

  if (file.length() > MAX_FILE_SIZE) {
    LOG("[FILE] reject: too large");
    server.send(413, "text/plain", "File too large");
    return;
  }
  addMessage(name, "[file]", "", room, file, fileType);
  server.send(200, "text/plain", "OK");
}

void handleEdit() {
  logRequest();
  String name    = server.arg("name");
  String newText = server.arg("text");
  String ip      = server.client().remoteIP().toString();

  if (isBanned(ip)) { LOG("[EDIT] reject: banned"); server.send(403, "text/plain", "Banned"); return; }
  int idx = findUserByName(name);
  if (idx < 0) { LOG("[EDIT] reject: '%s' not logged in", name.c_str()); server.send(401, "text/plain", "Not logged in"); return; }

  updateIp(name, ip);
  int msgNum = server.arg("idx").toInt();
  LOG("[EDIT] '%s' editing msg idx=%d newLen=%u", name.c_str(), msgNum, (unsigned)newText.length());
  for (int i = 0; i < msgCount; i++) {
    int pos = (msgIndex - msgCount + i + MAX_MESSAGES) % MAX_MESSAGES;
    if (i == msgNum && messages[pos].sender == name) {
      messages[pos].text = newText;
      saveMessages();
      LOG("[EDIT] applied to pos=%d", pos);
      break;
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleTyping() {
  logRequest();
  String name = server.arg("name");
  String ip   = server.client().remoteIP().toString();
  int idx = findUserByName(name);
  if (idx >= 0) { users[idx].typing = true; updateIp(name, ip); }
  LOG("[TYPING] '%s' typing=%d", name.c_str(), idx >= 0);
  server.send(200, "text/plain", "OK");
}

void handleMsgs() {
  logRequest();
  String ip = server.client().remoteIP().toString();
  if (isBanned(ip)) { LOG("[MSGS] reject: banned"); server.send(403, "application/json", "[]"); return; }

  String myName = server.arg("name");
  if (myName.length() == 0) {
    int myIdx = findUserByIp(ip);
    myName = (myIdx >= 0) ? users[myIdx].name : "";
  }

  String json = "[";
  int count = 0;
  for (int i = 0; i < msgCount; i++) {
    int idx = (msgIndex - msgCount + i + MAX_MESSAGES) % MAX_MESSAGES;
    if (messages[idx].to.length() == 0 ||
        messages[idx].sender == myName ||
        messages[idx].to == myName) {
      if (count > 0) json += ",";
      json += "{\"sender\":\"" + messages[idx].sender +
              "\",\"text\":\"" + messages[idx].text +
              "\",\"to\":\"" + messages[idx].to +
              "\",\"room\":\"" + messages[idx].room +
              "\",\"file\":\"" + messages[idx].file +
              "\",\"fType\":\"" + messages[idx].fileType +
              "\",\"ts\":" + String(messages[idx].ts) + "}";
      count++;
    }
  }
  json += "]";
  LOG("[MSGS] for '%s' -> %d msgs (%u bytes)", myName.c_str(), count, (unsigned)json.length());
  server.send(200, "application/json", json);
}

void handleTypingStatus() {
  // logging here would flood the monitor at 1 Hz — silently serve
  String ip = server.client().remoteIP().toString();
  String json = "[";
  for (int i = 0; i < userCount; i++) {
    if (users[i].typing && users[i].ip != ip) {
      if (json.length() > 1) json += ",";
      json += "\"" + users[i].name + "\"";
    }
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleCount() {
  // silently serve (polled at 2 Hz)
  server.send(200, "application/json", String(userCount));
}

void handleUsers() {
  logRequest();
  String ip = server.client().remoteIP().toString();
  String adminName = server.arg("name");
  int idx = adminName.length() > 0 ? findUserByName(adminName) : findUserByIp(ip);

  if (idx < 0 || !users[idx].isAdmin) {
    LOG("[USERS] reject: not admin");
    server.send(403, "application/json", "{\"error\":\"Admin only\"}");
    return;
  }
  updateIp(users[idx].name, ip);

  String json = "[";
  for (int i = 0; i < userCount; i++) {
    if (i > 0) json += ",";
    json += "{\"name\":\"" + users[i].name +
            "\",\"ip\":\"" + users[i].ip +
            "\",\"room\":\"" + users[i].room +
            "\",\"admin\":" + (users[i].isAdmin ? "true" : "false") + "}";
  }
  json += "]";
  LOG("[USERS] served %d entries to admin '%s'", userCount, users[idx].name.c_str());
  server.send(200, "application/json", json);
}

void handleActivity() {
  logRequest();
  String ip = server.client().remoteIP().toString();
  String adminName = server.arg("name");
  int idx = adminName.length() > 0 ? findUserByName(adminName) : findUserByIp(ip);

  if (idx < 0 || !users[idx].isAdmin) {
    LOG("[ACTIVITY] reject: not admin");
    server.send(403, "application/json", "[]");
    return;
  }
  String json = "[";
  for (int i = 0; i < actCount; i++) {
    int pos = (actIndex - actCount + i + MAX_ACTIVITIES) % MAX_ACTIVITIES;
    if (i > 0) json += ",";
    json += "{\"user\":\"" + activities[pos].user +
            "\",\"action\":\"" + activities[pos].action +
            "\",\"ts\":" + String(activities[pos].ts) + "}";
  }
  json += "]";
  LOG("[ACTIVITY] served %d entries", actCount);
  server.send(200, "application/json", json);
}

void handleRemove() {
  logRequest();
  String ip = server.client().remoteIP().toString();
  String adminName = server.arg("name");
  int idx = adminName.length() > 0 ? findUserByName(adminName) : findUserByIp(ip);

  if (idx < 0 || !users[idx].isAdmin) { LOG("[REMOVE] reject: not admin"); server.send(403, "text/plain", "Admin only"); return; }

  String targetName = server.arg("target");
  String targetIp   = server.arg("tip");
  LOG("[REMOVE] admin='%s' target='%s' tip=%s", adminName.c_str(), targetName.c_str(), targetIp.c_str());

  if (targetIp == ip) { LOG("[REMOVE] reject: self"); server.send(400, "text/plain", "Cannot remove yourself"); return; }

  for (int i = 0; i < userCount; i++) {
    if (users[i].name == targetName) {
      addActivity(targetName, "removed by admin");
      users[i] = users[userCount - 1];
      userCount--;
      break;
    }
  }
  if (bannedCount < MAX_BANNED) { banned[bannedCount] = targetIp; bannedCount++; LOG("[BAN] added %s (total %d)", targetIp.c_str(), bannedCount); }
  server.send(200, "text/plain", "OK");
}

void handleDeleteMsg() {
  logRequest();
  String ip = server.client().remoteIP().toString();
  String adminName = server.arg("name");
  int idx = adminName.length() > 0 ? findUserByName(adminName) : findUserByIp(ip);

  if (idx < 0 || !users[idx].isAdmin) { LOG("[DEL] reject: not admin"); server.send(403, "text/plain", "Admin only"); return; }

  int msgNum = server.arg("idx").toInt();
  LOG("[DEL] admin='%s' deleting msg idx=%d", adminName.c_str(), msgNum);
  for (int i = 0; i < msgCount; i++) {
    int pos = (msgIndex - msgCount + i + MAX_MESSAGES) % MAX_MESSAGES;
    if (i == msgNum) {
      messages[pos].text   = "[deleted]";
      messages[pos].sender = "Admin";
      messages[pos].file   = "";
      break;
    }
  }
  saveMessages();
  server.send(200, "text/plain", "OK");
}

void handleRoot() {
  logRequest();

  static const char PAGE_HEAD[] PROGMEM =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>LAN Chat</title>"
    "<style>"
    "body{font-family:sans-serif;background:#1a1a2e;color:#eee;margin:0;padding:10px}"
    "#login{text-align:center;margin-top:80px}"
    "#login input{padding:10px;margin:5px;border-radius:6px;border:none;width:200px}"
    "#login button{padding:10px 20px;border:none;border-radius:6px;background:#4fc3f7;font-weight:bold;cursor:pointer}"
    ".rooms{display:flex;gap:5px;margin-bottom:10px}"
    ".room-btn{padding:6px 14px;border:none;border-radius:4px;background:#16213e;color:#aaa;cursor:pointer;font-size:0.85em}"
    ".room-btn.active{background:#4fc3f7;color:#000;font-weight:bold}"
    "#chat{height:40vh;overflow-y:auto;padding:10px;background:#16213e;border-radius:8px;margin-bottom:8px}"
    ".msg{margin:8px 0;padding:8px;border-radius:6px;max-width:80%;position:relative;word-wrap:break-word}"
    ".mine{background:#0f3460;margin-left:auto}"
    ".theirs{background:#1a3a5c}"
    ".dm{background:#2d1b4e;border:1px solid #7c4dff}"
    ".sender{font-size:0.75em;color:#4fc3f7;cursor:pointer}"
    ".sender:hover{text-decoration:underline}"
    ".admin-tag{font-size:0.7em;color:#ff5252}"
    ".time{font-size:0.7em;color:#888;float:right;margin-left:8px}"
    ".del-btn{position:absolute;top:2px;right:4px;font-size:0.65em;color:#ff5252;cursor:pointer;display:none}"
    ".edit-btn{position:absolute;top:2px;right:14px;font-size:0.65em;color:#4fc3f7;cursor:pointer;display:none}"
    ".msg:hover .del-btn,.msg:hover .edit-btn{display:block}"
    ".msg img{max-width:200px;border-radius:4px;margin-top:4px}"
    ".file-link{color:#4fc3f7;font-size:0.85em;cursor:pointer}"
    "#typing{text-align:center;font-size:0.8em;color:#aaa;height:20px;margin-bottom:5px}"
    "#users{background:#16213e;padding:10px;border-radius:8px;margin-bottom:8px}"
    "#users h3{margin:0 0 8px;color:#4fc3f7}"
    ".user-row{display:flex;justify-content:space-between;align-items:center;padding:4px 0}"
    ".user-ip{font-size:0.75em;color:#888}"
    ".user-room{font-size:0.7em;color:#aaa}"
    ".remove-btn{background:#ff5252;color:#fff;border:none;padding:3px 8px;border-radius:4px;cursor:pointer;font-size:0.75em}"
    "#activity{background:#16213e;padding:10px;border-radius:8px;margin-bottom:8px}"
    "#activity h3{margin:0 0 8px;color:#4fc3f7}"
    ".act-item{font-size:0.8em;color:#aaa;padding:2px 0}"
    "input[type=text]{width:50%;padding:10px;border:none;border-radius:6px;font-size:1em}"
    "button.send{padding:10px 20px;border:none;border-radius:6px;background:#4fc3f7;color:#000;font-weight:bold;cursor:pointer}"
    "button.file-btn{padding:10px;border:none;border-radius:6px;background:#16213e;color:#4fc3f7;cursor:pointer}"
    "h2{text-align:center;color:#4fc3f7;margin:0 0 10px}"
    ".top-bar{display:flex;justify-content:space-between;align-items:center;margin-bottom:5px}"
    ".count{font-size:0.9em;color:#aaa}"
    ".role{font-size:0.8em;color:#aaa}"
    ".sound-toggle{font-size:0.8em;color:#4fc3f7;cursor:pointer}"
    ".dm-label{font-size:0.7em;color:#7c4dff}"
    "#search{width:100%;padding:6px 10px;border:none;border-radius:4px;background:#0f3460;color:#eee;font-size:0.85em;margin-bottom:8px}"
    ".edit-input{width:90%;padding:5px;border:1px solid #4fc3f7;border-radius:4px;background:#0f3460;color:#eee;font-size:0.9em}"
    "#jserr{display:none;background:#ff5252;color:#fff;padding:8px;border-radius:4px;margin:8px 0;font-size:0.85em;white-space:pre-wrap}"
    "</style></head><body>"
    "<div id='login'><h2>LAN Chat</h2>"
    "<div id='jserr'></div>"
    "<input type='text' id='uname' placeholder='Username'><br>"
    "<input type='password' id='upass' placeholder='Password (admin only)'><br>"
    "<button id='joinBtn' onclick='doLogin()'>Join</button>"
    "<p style='font-size:0.8em;color:#888'>Leave password empty for client</p></div>"

    "<div id='chatArea' style='display:none'>"
    "<div class='top-bar'>"
    "<span class='role' id='roleInfo'></span>"
    "<span class='sound-toggle' onclick='toggleSound()'>Sound: ON</span>"
    "<span class='count' id='userCount'>0 online</span>"
    "</div>"

    "<div class='rooms'>";

  static const char PAGE_MID[] PROGMEM =
    "</div>"
    "<input type='text' id='search' placeholder='Search messages...' oninput='doSearch(this.value)'>"
    "<div id='users' style='display:none'><h3>Users</h3><div id='userList'></div></div>"
    "<div id='activity' style='display:none'><h3>Activity</h3><div id='actList'></div></div>"
    "<div id='typing'></div><div id='chat'></div>"
    "<div style='display:flex;gap:8px'>"
    "<input type='text' id='msg' placeholder='Message' onkeypress='if(event.key==&quot;Enter&quot;)send()' oninput='sendTyping()'>"
    "<button class='file-btn' onclick='document.getElementById(&quot;fileInput&quot;).click()'>File</button>"
    "<input type='file' id='fileInput' style='display:none' onchange='sendFile(this)'>"
    "<button class='send' onclick='send()'>Send</button>"
    "</div></div>"
    "<script>";

  static const char PAGE_JS[] PROGMEM =
    "window.onerror=function(msg,url,ln,col,err){"
    "var e=document.getElementById('jserr');"
    "if(e){e.style.display='block';e.textContent='JS error: '+msg+' (line '+ln+')';}"
    "console.error('[JS] error:',msg,url,ln);};"
    "var myName='',isAdmin=false,lastIdx=0,soundOn=true,typingTimer=null,curDM='',curRoom='General',audioCtx=null;"
    "function escHtml(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;');}"
    "function getCtx(){if(!audioCtx)audioCtx=new (window.AudioContext||window.webkitAudioContext)();return audioCtx;}"
    "function beep(){if(!soundOn)return;try{var c=getCtx(),o=c.createOscillator(),g=c.createGain();o.connect(g);g.connect(c.destination);o.frequency.value=800;g.gain.value=0.1;o.start();o.stop(c.currentTime+0.1);}catch(e){}}"
    "function toggleSound(){soundOn=!soundOn;document.querySelector('.sound-toggle').textContent='Sound: '+(soundOn?'ON':'OFF');}"
    "function notify(t,b){if(document.hasFocus())return;if('Notification' in window && Notification.permission==='granted')new Notification(t,{body:b});}"

    "function doLogin(){"
    "console.log('[JS] doLogin clicked');"
    "var n=document.getElementById('uname').value.trim();"
    "var p=document.getElementById('upass').value;"
    "if(!n){alert('Enter a name');return;}"
    "var btn=document.getElementById('joinBtn');btn.textContent='Joining...';btn.disabled=true;"
    "var url='/login?name='+encodeURIComponent(n)+'&pass='+encodeURIComponent(p)+'&room=General';"
    "console.log('[JS] GET '+url);"
    "fetch(url).then(function(r){console.log('[JS] HTTP '+r.status);return r.json();}).then(function(d){"
    "console.log('[JS] reply',d);"
    "if(d.error){alert('Error: '+d.error);btn.textContent='Join';btn.disabled=false;return;}"
    "myName=d.name;isAdmin=d.admin;"
    "document.getElementById('login').style.display='none';"
    "document.getElementById('chatArea').style.display='block';"
    "document.getElementById('roleInfo').textContent=myName+(isAdmin?' [ADMIN]':' [Client]');"
    "if(isAdmin){document.getElementById('users').style.display='block';document.getElementById('activity').style.display='block';}"
    "poll();pollCount();"
    "}).catch(function(e){console.error('[JS] fetch failed:',e);alert('Network error: '+e);btn.textContent='Join';btn.disabled=false;});}"

    "function switchRoom(btn,room){curRoom=room;"
    "document.querySelectorAll('.room-btn').forEach(function(b){b.classList.remove('active');});"
    "btn.classList.add('active');document.getElementById('chat').innerHTML='';lastIdx=0;poll();}"

    "function fmtTime(ms){var d=new Date(ms);return ('0'+d.getHours()).slice(-2)+':'+('0'+d.getMinutes()).slice(-2);}"

    "function addMsg(m,idx){"
    "var chat=document.getElementById('chat');"
    "if(m.room!==curRoom && m.to.length===0)return;"
    "if(m.to && m.to!==myName)return;"
    "var div=document.createElement('div');"
    "if(m.to)div.className='msg dm';else if(m.sender===myName)div.className='msg mine';else div.className='msg theirs';"
    "var out='';"
    "if(isAdmin)out+='<span class=\"del-btn\" data-idx=\"'+idx+'\">x</span>';"
    "if(m.sender===myName)out+='<span class=\"edit-btn\" data-idx=\"'+idx+'\">Edit</span>';"
    "out+='<span class=\"sender\" data-sender=\"'+escHtml(m.sender)+'\">'+escHtml(m.sender)+'</span>';"
    "if(isAdmin && m.sender===myName)out+=' <span class=\"admin-tag\">[A]</span>';"
    "if(m.to)out+=' <span class=\"dm-label\">DM to '+escHtml(m.to)+'</span>';"
    "out+='<span class=\"time\">'+fmtTime(m.ts)+'</span> ';"
    "out+=escHtml(m.text);"
    "if(m.file){if(m.fType.indexOf('image')===0){out+='<br><img src=\"data:'+m.fType+';base64,'+m.file+'\">';}else{out+=' <a class=\"file-link\" href=\"data:'+m.fType+';base64,'+m.file+'\" download=\"file_'+m.ts+'\">[Download]</a>';}}"
    "div.innerHTML=out;"
    "var db=div.querySelector('.del-btn');if(db)db.onclick=function(){if(confirm('Delete?'))fetch('/delmsg?name='+encodeURIComponent(myName)+'&idx='+this.getAttribute('data-idx'));};"
    "var eb=div.querySelector('.edit-btn');if(eb)eb.onclick=function(){startEdit(this,idx,m.text);};"
    "var sb=div.querySelector('.sender');if(sb)sb.onclick=function(){startDM(this.getAttribute('data-sender'));};"
    "chat.appendChild(div);chat.scrollTop=chat.scrollHeight;"
    "if(m.sender!==myName && m.text!=='[deleted]')notify(m.sender,m.text);}"

    "function startEdit(el,idx,oldText){"
    "var span=el.parentElement;var input=document.createElement('input');input.className='edit-input';input.value=oldText;"
    "span.innerHTML='';span.appendChild(input);input.focus();"
    "input.onkeydown=function(e){if(e.key==='Enter'){fetch('/edit?name='+encodeURIComponent(myName)+'&text='+encodeURIComponent(input.value)+'&idx='+idx);setTimeout(poll,200);}else if(e.key==='Escape'){poll();}};}"

    "function doSearch(q){poll();}"
    "function startDM(name){if(name===myName)return;curDM=name;document.getElementById('msg').placeholder='DM to '+name;beep();}"
    "function sendTyping(){if(typingTimer)return;fetch('/typing?name='+encodeURIComponent(myName));typingTimer=setTimeout(function(){typingTimer=null;},2000);}"

    "function sendFile(input){var file=input.files[0];if(!file)return;if(file.size>50000){alert('Max 50KB');return;}"
    "var reader=new FileReader();"
    "reader.onload=function(){var b64=reader.result.split(',')[1];fetch('/sendfile?name='+encodeURIComponent(myName)+'&room='+encodeURIComponent(curRoom)+'&fType='+encodeURIComponent(file.type)+'&file='+encodeURIComponent(b64));};"
    "reader.readAsDataURL(file);input.value='';}"

    "function poll(){"
    "fetch('/msgs?name='+encodeURIComponent(myName)).then(function(r){return r.json();}).then(function(data){"
    "if(data.length>lastIdx)beep();for(var i=lastIdx;i<data.length;i++)addMsg(data[i],i);lastIdx=data.length;"
    "}).catch(function(e){console.warn('[JS] /msgs failed',e);});"
    "fetch('/typing').then(function(r){return r.json();}).then(function(names){document.getElementById('typing').textContent=names.length>0?names.join(', ')+' is typing...':'';}).catch(function(){});"
    "if(isAdmin){pollUsers();pollActivity();}}"

    "function pollCount(){fetch('/count').then(function(r){return r.text();}).then(function(n){document.getElementById('userCount').textContent=n+' online';}).catch(function(){});}"

    "function pollUsers(){fetch('/users?name='+encodeURIComponent(myName)).then(function(r){return r.json();}).then(function(data){"
    "if(data.error)return;var ul=document.getElementById('userList');ul.innerHTML='';"
    "data.forEach(function(u){var div=document.createElement('div');div.className='user-row';"
    "var sp=document.createElement('span');"
    "sp.innerHTML=escHtml(u.name)+(u.admin?' <span class=\"admin-tag\">[A]</span>':'')+' <span class=\"user-ip\">'+escHtml(u.ip)+'</span> <span class=\"user-room\">['+escHtml(u.room)+']</span>';"
    "div.appendChild(sp);"
    "if(u.name!==myName){var b=document.createElement('button');b.className='remove-btn';b.textContent='Remove';"
    "b.onclick=(function(nn,ip){return function(){if(confirm('Remove '+nn+'?'))fetch('/remove?name='+encodeURIComponent(myName)+'&target='+encodeURIComponent(nn)+'&tip='+encodeURIComponent(ip));};})(u.name,u.ip);"
    "div.appendChild(b);}ul.appendChild(div);});}).catch(function(){});}"

    "function pollActivity(){fetch('/activity?name='+encodeURIComponent(myName)).then(function(r){return r.json();}).then(function(data){"
    "if(!Array.isArray(data))return;var al=document.getElementById('actList');al.innerHTML='';"
    "for(var i=data.length-1;i>=Math.max(0,data.length-8);i--){var div=document.createElement('div');div.className='act-item';"
    "div.textContent=data[i].user+' '+data[i].action+' '+fmtTime(data[i].ts);al.appendChild(div);}}).catch(function(){});}"

    "function send(){var t=document.getElementById('msg').value.trim();if(!t)return;"
    "fetch('/send?name='+encodeURIComponent(myName)+'&text='+encodeURIComponent(t)+'&to='+encodeURIComponent(curDM)+'&room='+encodeURIComponent(curRoom));"
    "document.getElementById('msg').value='';document.getElementById('msg').placeholder='Message';curDM='';setTimeout(poll,200);}"

    "setInterval(poll,1000);setInterval(pollCount,2000);"
    "console.log('[JS] page loaded, doLogin defined:',typeof doLogin);";

  static const char PAGE_END[] PROGMEM =
    "</script></body></html>";

  String html;
  html.reserve(14000);
  html += FPSTR(PAGE_HEAD);

  // render room buttons dynamically
  for (int i = 0; i < ROOM_COUNT; i++) {
    html += String("<button class='room-btn") + (i == 0 ? " active" : "") +
            "' onclick='switchRoom(this,\"" + String(ROOMS[i]) + "\")'>" +
            String(ROOMS[i]) + "</button>";
  }

  html += FPSTR(PAGE_MID);
  html += FPSTR(PAGE_JS);
  html += FPSTR(PAGE_END);

  LOG("[HTTP] served index (%u bytes)", (unsigned)html.length());
  server.send(200, "text/html", html);
}
void handleNotFound() {
  String meth = (server.method() == HTTP_GET) ? "GET" : "POST";
  LOG("[404] %s %s from %s", meth.c_str(), server.uri().c_str(),
      server.client().remoteIP().toString().c_str());
  server.send(404, "text/plain", "Not found");
}

// ─── SETUP ───
void setup() {
  Serial.begin(115200);
  delay(1500);

  LOG("[BOOT] === starting ===");
  LOG("[BOOT] Free heap: %u bytes", (unsigned)ESP.getFreeHeap());

  fsReady = initFilesystem();
  if (fsReady) loadMessages();
  else         LOG("[BOOT] FS unavailable -> chat history will NOT persist");

  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  LOG("[WIFI] connecting to '%s'", ssid);
  WiFi.begin(ssid, password);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500); Serial.print("."); tries++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    LOG("[WIFI] connected  IP=%s  RSSI=%d dBm  channel=%d",
        WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
    LOG("[BOOT] Chat Server: http://%s", WiFi.localIP().toString().c_str());
  } else {
    LOG("[WIFI] FAILED after %d tries (status=%d)", tries, WiFi.status());
    LOG("[WIFI] SSID='%s' — check name/password and 2.4 GHz band", ssid);
  }

  server.on("/", handleRoot);
  server.on("/login", HTTP_GET, handleLogin);
  server.on("/send", HTTP_GET, handleSend);
  server.on("/sendfile", HTTP_GET, handleSendFile);
  server.on("/edit", HTTP_GET, handleEdit);
  server.on("/typing", HTTP_GET, handleTyping);
  server.on("/typingstatus", HTTP_GET, handleTypingStatus);
  server.on("/msgs", handleMsgs);
  server.on("/count", handleCount);
  server.on("/users", handleUsers);
  server.on("/activity", handleActivity);
  server.on("/remove", HTTP_GET, handleRemove);
  server.on("/delmsg", HTTP_GET, handleDeleteMsg);
  server.onNotFound(handleNotFound);
  server.begin();
  LOG("[BOOT] HTTP server started on port 80");
}

void loop() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 15000) {
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED) checkWiFi();
    else LOG("[WIFI] heartbeat  IP=%s  RSSI=%d  heap=%u",
             WiFi.localIP().toString().c_str(), WiFi.RSSI(), (unsigned)ESP.getFreeHeap());
  }
  server.handleClient();
}