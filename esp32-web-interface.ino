/* 
  FSWebServer - Example WebServer with SPIFFS backend for esp8266
  Copyright (c) 2015 Hristo Gochkov. All rights reserved.
  This file is part of the ESP8266WebServer library for Arduino environment.
 
  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.
  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.
  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
  
  upload the contents of the data folder with MkSPIFFS Tool ("ESP8266 Sketch Data Upload" in Tools menu in Arduino IDE)
  or you can upload the contents of a folder if you CD in that folder and run the following command:
  for file in `ls -A1`; do curl -F "file=@$PWD/$file" esp8266fs.local/edit; done
  
  access the sample web page at http://esp8266fs.local
  edit the page by going to http://esp8266fs.local/edit
*/
/*
 * This file is part of the esp8266 web interface
 *
 * Copyright (C) 2018 Johannes Huebner <dev@johanneshuebner.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <FS.h>
#include <Ticker.h>

#include <SD_MMC.h>
#include "RTClib.h"
#include <ESP32Time.h>
#include <time.h>
#include "driver/uart.h"

#define DBG_OUTPUT_PORT Serial2
#define INVERTER_PORT UART_NUM_0
#define INVERTER_RX 3
#define INVERTER_TX 1
#define UART_TIMEOUT (100 / portTICK_PERIOD_MS)
#define UART_MESSBUF_SIZE 100
#define LED_BUILTIN 13 //clashes with SDIO, need to change to suit hardware and uncomment lines

#define RESERVED_SD_SPACE 2000000000
#define SDIO_BUFFER_SIZE 16384
#define FLUSH_WRITES 60 //flush file every 60 blocks

#define MAX_SD_FILES 200

#define LOG_DELAY_VAL 10000

#define	RTC_USED_PCF8523	0
#define	RTC_USED_DS3231		1
#define RTC_USED			    RTC_USED_DS3231

//HardwareSerial Inverter(INVERTER_PORT);

const char* host = "inverter";
bool fastUart = false;
bool fastUartAvailable = true;
char uartMessBuff[UART_MESSBUF_SIZE];

WebServer server(80);
HTTPUpdateServer updater;
//holds the current upload
File fsUploadFile;
Ticker sta_tick;

//SWD over ESP8266
/*
  https://github.com/scanlime/esp8266-arm-swd
*/
#include <StreamString.h>

#if RTC_USED == RTC_USED_DS3231
	RTC_PCF8523 ext_rtc;
#elif RTC_USED == RTC_USED_DS3231
	RTC_DS3231 ext_rtc;
#else
	#error ("You have to define a availabled RTC IC")
#endif

ESP32Time int_rtc;
bool haveRTC = false;
bool haveSDCard = false;
bool fastLoggingEnabled = true;
bool fastLoggingActive = false;
uint8_t SDIObuffer[SDIO_BUFFER_SIZE];
uint16_t indexSDIObuffer = 0;
uint16_t blockCountSD = 0;
File dataFile;
int startLogAttempt = 0;

bool createNextSDFile()
{
  char filename[50];

  uint32_t nextFileIndex = deleteOldest(RESERVED_SD_SPACE);

  if(haveRTC)
    nextFileIndex = 0; //have a date so restart index from 0 (still needed in case serial stream fails to start)

  do
  {
    if(haveRTC)
      snprintf(filename, 50, "/%d-%02d-%02d-%02d-%02d-%02d_%d.bin", int_rtc.getYear(), int_rtc.getMonth(), int_rtc.getDay(), int_rtc.getHour(), int_rtc.getMinute(), int_rtc.getSecond(), nextFileIndex++);
    else
      snprintf(filename, 50, "/%010d.bin", nextFileIndex++);
  }
  while(SD_MMC.exists(filename));
      
  dataFile = SD_MMC.open(filename, FILE_WRITE);
  if (dataFile) 
  {
    dataFile.flush(); //make sure FAT updated for debugging purposes
    DBG_OUTPUT_PORT.println("Created file: " + String(filename)); 
    return true;
  }
  else
    return false;
}

uint32_t deleteOldest(uint64_t spaceRequired)
{
  time_t oldestTime = 0;
  File root, file;
  String oldestFileName;
  uint64_t spaceRem;
  time_t t;
  uint32_t nextIndex = 0;
  uint32_t fileCount = 0;
  
  spaceRem = SD_MMC.totalBytes() - SD_MMC.usedBytes();

  DBG_OUTPUT_PORT.println("Space Required = " + formatBytes(spaceRequired));
  DBG_OUTPUT_PORT.println("Space Remaining = " + formatBytes(spaceRem));
  
  do
  {
    root = SD_MMC.open("/");
    
    oldestTime = 0;
    fileCount = 0;
    while(file = root.openNextFile())
    {
      if(haveRTC)
        t = file.getLastWrite();
      else
      {
        String fname = file.name();
        fname.remove(0,1); //lose starting /
        t = fname.toInt()+1; //make sure 0 special case isnt used
        if(t > nextIndex)
          nextIndex = t;
      }
      if(!file.isDirectory())
      {
        fileCount++;
        if((oldestTime==0) || (t<oldestTime))
        {
          oldestTime = t;
          oldestFileName = "/";
          oldestFileName += file.name();
        }
      }
      file.close();
    }  
    root.close();

    if((spaceRem < spaceRequired) || (fileCount >= MAX_SD_FILES))
    {
      if(oldestFileName.length() > 0)
      {
        
        if(SD_MMC.remove(oldestFileName))
          DBG_OUTPUT_PORT.println("Deleted file: " + oldestFileName);
        else
          DBG_OUTPUT_PORT.println("Couldn't delete: " + oldestFileName);
      }
      else
      {
        DBG_OUTPUT_PORT.println("No files found, can't free space");
        break;//no files so can do no more
      }
    }

    spaceRem = SD_MMC.totalBytes() - SD_MMC.usedBytes();
  } while((spaceRem < spaceRequired) || (fileCount >= MAX_SD_FILES));


  return(nextIndex);
}

//format bytes
String formatBytes(uint64_t bytes){
  if (bytes < 1024){
    return String(bytes)+"B";
  } else if(bytes < (1024 * 1024)){
    return String(bytes/1024.0)+"KB";
  } else if(bytes < (1024 * 1024 * 1024)){
    return String(bytes/1024.0/1024.0)+"MB";
  } else {
    return String(bytes/1024.0/1024.0/1024.0)+"GB";
  }
}

String getContentType(String filename){
  if(server.hasArg("download")) return "application/octet-stream";
  else if(filename.endsWith(".bin")) return "application/octet-stream";
  else if(filename.endsWith(".htm")) return "text/html";
  else if(filename.endsWith(".html")) return "text/html";
  else if(filename.endsWith(".css")) return "text/css";
  else if(filename.endsWith(".js")) return "application/javascript";
  else if(filename.endsWith(".png")) return "image/png";
  else if(filename.endsWith(".gif")) return "image/gif";
  else if(filename.endsWith(".jpg")) return "image/jpeg";
  else if(filename.endsWith(".ico")) return "image/x-icon";
  else if(filename.endsWith(".xml")) return "text/xml";
  else if(filename.endsWith(".pdf")) return "application/x-pdf";
  else if(filename.endsWith(".zip")) return "application/x-zip";
  else if(filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}

bool handleFileRead(String path){
  //DBG_OUTPUT_PORT.println("handleFileRead: " + path);
  if(path.endsWith("/")) path += "index.html";
  String contentType = getContentType(path);
  String pathWithGz = path + ".gz";
  if(SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)){
    if(SPIFFS.exists(pathWithGz))
      path += ".gz";
    File file = SPIFFS.open(path, "r");
    size_t sent = server.streamFile(file, contentType);
    file.close();
    return true;
  }
  //try download from the sdcard
  if (haveSDCard) {
    DBG_OUTPUT_PORT.print("handleFileRead Trying SD Card: ");
    DBG_OUTPUT_PORT.println(path);
    DBG_OUTPUT_PORT.print("SD_MMC.exists: ");
    DBG_OUTPUT_PORT.println(SD_MMC.exists( path));

    if (SD_MMC.exists(path)) {
      File file = SD_MMC.open(path, "r");
      size_t sent = server.streamFile(file, contentType);
      file.close();
    return true;
    }
  }
  return false;
}

void handleFileUpload(){
  if(server.uri() != "/edit") return;
  HTTPUpload& upload = server.upload();
  if(upload.status == UPLOAD_FILE_START){
    String filename = upload.filename;
    if(!filename.startsWith("/")) filename = "/"+filename;
    //DBG_OUTPUT_PORT.print("handleFileUpload Name: "); DBG_OUTPUT_PORT.println(filename);
    fsUploadFile = SPIFFS.open(filename, "w");
    filename = String();
  } else if(upload.status == UPLOAD_FILE_WRITE){
    //DBG_OUTPUT_PORT.print("handleFileUpload Data: "); DBG_OUTPUT_PORT.println(upload.currentSize);
    if(fsUploadFile)
      fsUploadFile.write(upload.buf, upload.currentSize);
  } else if(upload.status == UPLOAD_FILE_END){
    if(fsUploadFile)
      fsUploadFile.close();
    //DBG_OUTPUT_PORT.print("handleFileUpload Size: "); DBG_OUTPUT_PORT.println(upload.totalSize);
  }
}

void handleFileDelete(){
  if(server.args() == 0) return server.send(500, "text/plain", "BAD ARGS");
  String path = server.arg(0);
  //DBG_OUTPUT_PORT.println("handleFileDelete: " + path);
  if(path == "/")
    return server.send(500, "text/plain", "BAD PATH");
  if(!SPIFFS.exists(path))
    return server.send(404, "text/plain", "FileNotFound");
  SPIFFS.remove(path);
  server.send(200, "text/plain", "");
  path = String();
}

void handleFileCreate(){
  if(server.args() == 0)
    return server.send(500, "text/plain", "BAD ARGS");
  String path = server.arg(0);
  //DBG_OUTPUT_PORT.println("handleFileCreate: " + path);
  if(path == "/")
    return server.send(500, "text/plain", "BAD PATH");
  if(SPIFFS.exists(path))
    return server.send(500, "text/plain", "FILE EXISTS");
  File file = SPIFFS.open(path, "w");
  if(file)
    file.close();
  else
    return server.send(500, "text/plain", "CREATE FAILED");
  server.send(200, "text/plain", "");
  path = String();
}

void handleRTCNow() {
  String output = "{ \"now\":\"";
  if (haveRTC) {
    DateTime t = ext_rtc.now();
    output += t.timestamp();
  } else {
    output += "NO RTC";
  }
  output += "\"}";
  server.send(200, "text/json", output);
}

void handleRTCSet() {

 if (server.hasArg("timestamp")) {
    String timestamp = server.arg("timestamp");
    server.send(200, "text/json", "{\"result\":\"" + timestamp + "\"}");
    DateTime now = DateTime(timestamp.toInt());
    ext_rtc.adjust(now);
    int_rtc.setTime(now.unixtime());  
    handleRTCNow();
 } else {
    server.send(500, "text/json", "{\"result\":\"timestamp missing\"}");

 }
}
void handleSdCardDeleteAll() {
    if (haveSDCard) {
      File root, file;
      if (haveSDCard) {
        root = SD_MMC.open("/");
        while(file = root.openNextFile())
        { 
          String filename = file.name();
          if(SD_MMC.remove("/" + filename))
            DBG_OUTPUT_PORT.println("Deleted file: " + filename);
          else
            DBG_OUTPUT_PORT.println("Couldn't delete: " + filename);
          }
      }
    }

    server.send(200, "text/json", "{\"result\": \"done\"}");

}
void handleSdCardList() {
  
  if (!haveSDCard) {
    server.send(200, "text/json", "{\"error\": \"No SD Card\"}");
    return;
  }
  File root = SD_MMC.open("/");
  if(!root){
    server.send(200, "text/json", "{\"error\": \"Failed to open directory\"}");
    return;
  }
  if(!root.isDirectory()){
    server.send(200, "text/json", "{\"error\": \"Root is not a directory\"}");
    return;
  }
  File sdFile = root.openNextFile();
  String output = "[";
  int count = 0;
  while(sdFile && count < 200){
    if (output != "[") output += ',';
    output += "\"";
    output += String(sdFile.name());
    output += "\"";
    sdFile = root.openNextFile();

    count++;
  }
  output += "]";
  server.send(200, "text/json", output);
  return;
}

void handleFileList() {
  String path = "/";
  if(server.hasArg("dir")) 
    String path = server.arg("dir");
  //DBG_OUTPUT_PORT.println("handleFileList: " + path);
  File root = SPIFFS.open(path);
  String output = "[";

  if(!root){
    //DBG_OUTPUT_PORT.print("- failed to open directory");
    return;
  }

  File file = root.openNextFile();
  while(file){
    if (output != "[") output += ',';
    bool isDir = false;
    output += "{\"type\":\"";
    output += file.isDirectory()?"dir":"file";
    output += "\",\"name\":\"";
    output += String(file.name());
    output += "\"}";
    file = root.openNextFile();
  }
  
  output += "]";
  server.send(200, "text/json", output);
}

// static void sendCommand(String cmd)
// {
//   DBG_OUTPUT_PORT.println("Sending '" + cmd + "' to inverter");
//   Inverter.print("\n");
//   delay(1);
//   while(Inverter.available())
//     Inverter.read(); //flush all previous output
//   Inverter.print(cmd);
//   Inverter.print("\n");
//   Inverter.readStringUntil('\n'); //consume echo  
// }

void uart_readUntill(char val)
{
  int retVal;
  do
  {
    retVal = uart_read_bytes(INVERTER_PORT, uartMessBuff, 1, UART_TIMEOUT);
  }
  while((retVal>0) && (uartMessBuff[0] != val));
}

bool uart_readStartsWith(const char *val)
{
  bool retVal = false;
  int rxBytes = uart_read_bytes(INVERTER_PORT, uartMessBuff, strnlen(val,UART_MESSBUF_SIZE), UART_TIMEOUT);
  if(rxBytes >= strnlen(val,UART_MESSBUF_SIZE))
  {
    if(strncmp(val, uartMessBuff, strnlen(val,UART_MESSBUF_SIZE))==0)
      retVal = true;
    uartMessBuff[rxBytes] = 0;
    DBG_OUTPUT_PORT.println(uartMessBuff);
  }
  return retVal;
}



static void sendCommand(String cmd)
{
  DBG_OUTPUT_PORT.println("Sending '" + cmd + "' to inverter");
  //Inverter.print("\n");
  uart_write_bytes(INVERTER_PORT, "\n", 1);
  delay(1);
  //while(Inverter.available())
  //  Inverter.read(); //flush all previous output
  uart_flush(INVERTER_PORT);
  //Inverter.print(cmd);
  uart_write_bytes(INVERTER_PORT, cmd.c_str(), cmd.length());
  //Inverter.print("\n");
  uart_write_bytes(INVERTER_PORT, "\n", 1);
  //Inverter.readStringUntil('\n'); //consume echo  
  uart_readUntill('\n');
}

static void handleCommand() {
  const int cmdBufSize = 128;
  if(!server.hasArg("cmd")) {server.send(500, "text/plain", "BAD ARGS"); return;}

  String cmd = server.arg("cmd").substring(0, cmdBufSize);
  int repeat = 0;
  char buffer[255];
  size_t len = 0;
  String output;

  if (server.hasArg("repeat"))
    repeat = server.arg("repeat").toInt();

  if (!fastUart && fastUartAvailable)
  {
    sendCommand("fastuart");
    if (uart_readStartsWith("OK"))
    {
      //Inverter.begin(921600, SERIAL_8N1, INVERTER_RX, INVERTER_TX);
      //Inverter.updateBaudRate(921600);
      uart_set_baudrate(INVERTER_PORT, 921600);
      fastUart = true;
    }
    else
    {
      fastUartAvailable = false;
    }
  }

  sendCommand(cmd);
  do {
    memset(buffer,0,sizeof(buffer));
    //len = Inverter.readBytes(buffer, sizeof(buffer) - 1);
    len = uart_read_bytes(INVERTER_PORT, buffer, sizeof(buffer), UART_TIMEOUT);
    if(len > 0) output.concat(buffer, len);// += buffer;

    if (repeat)
    {
      repeat--;
      //Inverter.print("!");
      uart_write_bytes(INVERTER_PORT, "!", 1);
      //Inverter.readBytes(buffer, 1); //consume "!"
      uart_read_bytes(INVERTER_PORT, buffer, 1, UART_TIMEOUT);
    }
  } while (len > 0);
  DBG_OUTPUT_PORT.println(output);
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.send(200, "text/json", output);
}

static uint32_t crc32_word(uint32_t Crc, uint32_t Data)
{
  int i;

  Crc = Crc ^ Data;

  for(i=0; i<32; i++)
    if (Crc & 0x80000000)
      Crc = (Crc << 1) ^ 0x04C11DB7; // Polynomial used in STM32
    else
      Crc = (Crc << 1);

  return(Crc);
}

static uint32_t crc32(uint32_t* data, uint32_t len, uint32_t crc)
{
   for (uint32_t i = 0; i < len; i++)
      crc = crc32_word(crc, data[i]);
   return crc;
}


static void handleUpdate()
{
  if(!server.hasArg("step") || !server.hasArg("file")) {server.send(500, "text/plain", "BAD ARGS"); return;}
  size_t PAGE_SIZE_BYTES = 1024;
  int step = server.arg("step").toInt();
  File file = SPIFFS.open(server.arg("file"), "r");
  uint8_t pages = (file.size() + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES;
  String message;

  if (server.hasArg("pagesize"))
  {
    PAGE_SIZE_BYTES = server.arg("pagesize").toInt();
  }

  if (step == -1)
  {
    //int c;
    char c;
    sendCommand("reset");

    if (fastUart)
    {
      //Inverter.begin(115200, SERIAL_8N1, INVERTER_RX, INVERTER_TX);
      //Inverter.updateBaudRate(115200);
      uart_set_baudrate(INVERTER_PORT, 115200);
      fastUart = false;
      fastUartAvailable = true; //retry after reboot
    }
    do {
      //c = Inverter.read();
      uart_read_bytes(INVERTER_PORT, &c, 1, UART_TIMEOUT);
    } while (c != 'S' && c != '2');

    if (c == '2') //version 2 bootloader
    {
      //Inverter.write(0xAA); //Send magic
      c = 0xAA;
      uart_write_bytes(INVERTER_PORT, &c, 1);
      //while (Inverter.read() != 'P');
      do {
        uart_read_bytes(INVERTER_PORT, &c, 1, UART_TIMEOUT);
      } while (c != 'S');
    }
    
    //Inverter.write(pages);
    uart_write_bytes(INVERTER_PORT, &pages, 1);
    //while (Inverter.read() != 'P');
    do {
      uart_read_bytes(INVERTER_PORT, &c, 1, UART_TIMEOUT);
    } while (c != 'P');
    message = "reset";
  }
  else
  {
    bool repeat = true;
    file.seek(step * PAGE_SIZE_BYTES);
    char buffer[PAGE_SIZE_BYTES];
    size_t bytesRead = file.readBytes(buffer, sizeof(buffer));

    while (bytesRead < PAGE_SIZE_BYTES)
      buffer[bytesRead++] = 0xff;
    
    uint32_t crc = crc32((uint32_t*)buffer, PAGE_SIZE_BYTES / 4, 0xffffffff);

    while (repeat)
    {
      //Inverter.write(buffer, sizeof(buffer));
      uart_write_bytes(INVERTER_PORT, buffer, sizeof(buffer));
      //while (!Inverter.available());
      char res;// = Inverter.read();
      while(uart_read_bytes(INVERTER_PORT, &res, 1, UART_TIMEOUT)<=0);

      if ('C' == res) {
        //Inverter.write((char*)&crc, sizeof(uint32_t));
        uart_write_bytes(INVERTER_PORT, (char*)&crc, sizeof(uint32_t));
        //while (!Inverter.available());
        //res = Inverter.read();
        while(uart_read_bytes(INVERTER_PORT, &res, 1, UART_TIMEOUT)<=0);
      }

      switch (res) {
        case 'D':
          message = "Update Done";
          repeat = false;
          fastUartAvailable = true;
          break;
        case 'E':
          //while (Inverter.read() != 'T');
          do {
            uart_read_bytes(INVERTER_PORT, uartMessBuff, 1, UART_TIMEOUT);
          } while (uartMessBuff[0] != 'T');
          break;
        case 'P':
          message = "Page write success";
          repeat = false;
          break;
        default:
        case 'T':
          break;
      }
    }
  }
  server.send(200, "text/json", "{ \"message\": \"" + message + "\", \"pages\": " + pages + " }");
  file.close();
}

static void handleWifi()
{
  bool updated = true;
  if(server.hasArg("apSSID") && server.hasArg("apPW")) 
  {
    WiFi.softAP(server.arg("apSSID").c_str(), server.arg("apPW").c_str());
  }
  else if(server.hasArg("staSSID") && server.hasArg("staPW")) 
  {
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(server.arg("staSSID").c_str(), server.arg("staPW").c_str());
  }
  else
  {
    File file = SPIFFS.open("/wifi.html", "r");
    String html = file.readString();
    file.close();
    html.replace("%staSSID%", WiFi.SSID());
    html.replace("%apSSID%", WiFi.softAPSSID());
    html.replace("%staIP%", WiFi.localIP().toString());
    server.send(200, "text/html", html);
    updated = false;
  }

  if (updated)
  {
    File file = SPIFFS.open("/wifi-updated.html", "r");
    size_t sent = server.streamFile(file, getContentType("wifi-updated.html"));
    file.close();    
  }
}

static void handleBaud()
{
  if (fastUart)
    server.send(200, "text/html", "fastUart on");
  else
    server.send(200, "text/html", "fastUart off");
}

void staCheck(){
  sta_tick.detach();
  if(!(uint32_t)WiFi.localIP()){
    WiFi.mode(WIFI_AP); //disable station mode
  }
}

void setup(void){
  DBG_OUTPUT_PORT.begin(115200);
  //Inverter.setRxBufferSize(50000);
  //Inverter.begin(115200, SERIAL_8N1, INVERTER_RX, INVERTER_TX);
  //Need to use low level Espressif IDF API instead of Serial to get high enough data rates
  uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE};

  uart_param_config(INVERTER_PORT, &uart_config);
  uart_set_pin(INVERTER_PORT, INVERTER_TX, INVERTER_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  uart_driver_install(INVERTER_PORT, SDIO_BUFFER_SIZE * 3, 0, 0, NULL, 0); //x3 allows twice card write size to buffer while writes
  delay(100); 
  

  //check for external RTC and if present use to initialise on-chip RTC
  if (ext_rtc.begin())
  {
    haveRTC = true;
    DBG_OUTPUT_PORT.println("External RTC found");
    
#if RTC_USED == RTC_USED_PCF8523
	if (! ext_rtc.initialized() || ext_rtc.lostPower())
#elif RTC_USED == RTC_USED_DS3231
	if (ext_rtc.lostPower()) 
#else
	#error ("You have to define a availabled RTC IC")
#endif
    {
      DBG_OUTPUT_PORT.println("RTC is NOT initialized, setting to build time");
      ext_rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }

#if RTC_USED == RTC_USED_PCF8523
	ext_rtc.start();
#elif RTC_USED == RTC_USED_DS3231

#else
	#error ("You have to define a availabled RTC IC")
#endif
    DateTime now = ext_rtc.now();
    int_rtc.setTime(now.unixtime());  
  }
  else
    DBG_OUTPUT_PORT.println("No RTC found, defaulting to sequential file names"); 

  //initialise SD card in SDIO mode
  //if (SD_MMC.begin("/sdcard", true, false, 40000, 5U)) {
  if (SD_MMC.begin()) {
    DBG_OUTPUT_PORT.println("Started SD_MMC");    
    haveSDCard = true;    
  }
  else
    DBG_OUTPUT_PORT.println("Couldn't start SD_MMC");  

  //Start SPI Flash file system
  SPIFFS.begin();

  //WIFI INIT
  #ifdef WIFI_IS_OFF_AT_BOOT
    enableWiFiAtBootTime();
  #endif
  WiFi.mode(WIFI_AP_STA);
  //WiFi.setPhyMode(WIFI_PHY_MODE_11B);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);//25); //dbm
  WiFi.begin();
  sta_tick.attach(10, staCheck);
  
  MDNS.begin(host);

  updater.setup(&server);
  
  //SERVER INIT
  ArduinoOTA.setHostname(host);
  ArduinoOTA.begin();
  //list directory
  server.on("/list", HTTP_GET, handleFileList);

  server.on("/rtc/now", HTTP_GET, handleRTCNow);
  server.on("/rtc/set", HTTP_POST, handleRTCSet);
  server.on("/sdcard/list", HTTP_GET, handleSdCardList);
  server.on("/sdcard/deleteAll", HTTP_GET, handleSdCardDeleteAll);

  //load editor
  server.on("/edit", HTTP_GET, [](){
    if(!handleFileRead("/edit.htm")) server.send(404, "text/plain", "FileNotFound");
  });
  //create file
  server.on("/edit", HTTP_PUT, handleFileCreate);
  //delete file
  server.on("/edit", HTTP_DELETE, handleFileDelete);
  //first callback is called after the request has ended with all parsed arguments
  //second callback handles file uploads at that location
  server.on("/edit", HTTP_POST, [](){ server.send(200, "text/plain", ""); }, handleFileUpload);

  server.on("/wifi", handleWifi);
  server.on("/cmd", handleCommand);
  server.on("/fwupdate", handleUpdate);
  server.on("/baud", handleBaud);
  server.on("/version", [](){ server.send(200, "text/plain", "1.1.R"); });
  
  //called when the url is not defined here
  //use it to load content from SPIFFS
  server.onNotFound([](){
    if(!handleFileRead(server.uri()))
    {
      server.sendHeader("Refresh", "6; url=/update");
      server.send(404, "text/plain", "FileNotFound");
    }
  });

  server.begin();
  server.client().setNoDelay(1);

  MDNS.addService("http", "tcp", 80);
}

void binaryLoggingStart()
{
  if(createNextSDFile())
  {
    sendCommand(""); //flush out buffer in case just had power up
    delay(10);
    sendCommand("binarylogging 1"); //send start logging command to inverter
    delayMicroseconds(200);
    if (uart_readStartsWith("OK"))
    {
      uart_set_baudrate(INVERTER_PORT, 2250000);
      fastLoggingActive = true;
      DBG_OUTPUT_PORT.println("Binary logging started");
    }
    else //no response - in case it did actually switch but we missed response send the turn off command
    {
      dataFile.close();
      uart_set_baudrate(INVERTER_PORT, 2250000);
      uart_write_bytes(INVERTER_PORT, "\n", 1);
      delay(1);
      uart_write_bytes(INVERTER_PORT, "binarylogging 0", strnlen("binarylogging 0", UART_MESSBUF_SIZE));
      uart_write_bytes(INVERTER_PORT, "\n", 1);
      uart_wait_tx_done(INVERTER_PORT, UART_TIMEOUT);
      uart_set_baudrate(INVERTER_PORT, 115200);
    }
    delay(10);
    uart_flush(INVERTER_PORT);
  }
}

void binaryLoggingStop()
{
  uart_write_bytes(INVERTER_PORT, "\n", 1);
  delay(1);
  uart_write_bytes(INVERTER_PORT, "binarylogging 0", strnlen("binarylogging 0", UART_MESSBUF_SIZE));
  uart_write_bytes(INVERTER_PORT, "\n", 1);
  uart_wait_tx_done(INVERTER_PORT, UART_TIMEOUT);
  uart_set_baudrate(INVERTER_PORT, 115200);
  delay(100);
  uart_flush(INVERTER_PORT);
  //data should now have stopped so send command again and check response
  sendCommand("binarylogging 0");
  if (uart_readStartsWith("OK"))
  {
    uart_set_baudrate(INVERTER_PORT, 115200);
    fastUart = false;
    fastLoggingActive = false;
    dataFile.flush(); //make sure up to date
    dataFile.close();
    DBG_OUTPUT_PORT.println("Binary logging terminated");
  }
  else
  { //assume still logging so try again next time round
    uart_set_baudrate(INVERTER_PORT, 2250000);
  }
  delay(10);
  uart_flush(INVERTER_PORT);
}

 
void loop(void){
  // note: ArduinoOTA.handle() calls MDNS.update();
  server.handleClient();
  ArduinoOTA.handle();

  if((WiFi.softAPgetStationNum() > 0) || (WiFi.status() == WL_CONNECTED))
  { //have connections so stop logging
    startLogAttempt=0; //restart log attempts when next disconnected
    if(fastLoggingActive) //was it active last pass
      binaryLoggingStop();
  }
  else
  { //no connections so log
    if(fastLoggingActive) //already active, just carry on writing data
    {
      int spaceAvail = SDIO_BUFFER_SIZE - indexSDIObuffer;
      int bytesRead = uart_read_bytes(INVERTER_PORT, &SDIObuffer[indexSDIObuffer], spaceAvail, UART_TIMEOUT);
      if(bytesRead > 0)
      {
        indexSDIObuffer += bytesRead;
        if(indexSDIObuffer >= SDIO_BUFFER_SIZE)
        {
          dataFile.write(SDIObuffer, SDIO_BUFFER_SIZE);
          indexSDIObuffer = 0;
          blockCountSD++;
          if(blockCountSD >= FLUSH_WRITES)
          {
            blockCountSD = 0;
            dataFile.flush();
          }
        }
      }
    }
    else //not active so start
    {
      if(haveSDCard && fastLoggingEnabled && (startLogAttempt < 3) && (millis() > LOG_DELAY_VAL))
      {
        startLogAttempt++;
        binaryLoggingStart();
      }
    }
  }
}
