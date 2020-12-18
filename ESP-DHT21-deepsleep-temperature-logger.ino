//  Temperature/humidity logger using DHT-21 (AM2301)
//  Log temperatures to Google Sheets
//  Alex Tatistcheff (alext@pobox.com)
//
// Uses deep sleep to save power
// Connections
// D0 (GPIO16) jumper to RST to allow deepsleep to wake up.  Must disconnect to program!
// D4 (GIPIO2) connects to DHT21 data
// GPIO4 - DHT21 pwr - this is the base of a 2N2222 transistor
// Gnd - Gnd 
// There's a voltage divider feeding pin A0.  A0 is connected to Vcc via a 1M resistor and also connected to Gnd via a 100K resistor.
// This will then report the voltage at about 100 times the actual Vcc, i.e. 310 is about 3.1 volts.
// GPIO14 is connected to one side of a button, the other side is to Gnd.  Holding the button on power up will clear the WiFi settings.

// Add parameters from Wifi Config
// - Deep sleep interval
// - Sensor ID (Name of sheet for temps at Google)
// Note: When flashing a new ESP-12F you have to initialize the SPIFFS first!  See notes for procedure.

// Todo:
// Include OTA function
// Use a button that would be held on startup to put the device into OTA mode
// Probably need two buttons, one reset and the other OTA hold.  Maybe even flash 
// the on board LED when we're in OTA.  (LED_BUILTIN)
// OTA seems a bit buggy, maybe not worry about this one so much.

#include <FS.h>   
#include <ArduinoJson.h> 
#include "WiFiManager.h"
#include <ESP8266WiFi.h>
#include "HTTPSRedirect.h"
#include "DebugMacros.h"
#include <DHT.h>
#define DHTTYPE DHT21
#define DHTPIN  2
#define DHTPWR 4
#define WIFI_CLR 14
#define PROJECT_NAME "ESP-TEMP/HUM"

float humidity, temp_f;  // Values read from sensor
char sensorID[15] = "sensor_new";  // This is the name of the tab on the Datalogger sheet, this sheet must exist!
int deepSleepSecs = 12000;      // Default setting for deep sleep
char strdeepSleepSecs[8] = "1200";  //used for reading from json
int analogValue;  // Analog pin reading

// The ID below comes from Google Sheets. See how to get it here: http://embedded-lab.com/blog/post-data-google-sheets-using-esp8266
char GScriptID[60] = "scrip_id_here";  // Default setting for GScriptID
//char GScriptID[60] = "<YOUR-GSCRIPT-ID-HERE>";  // Default setting for GScriptID

const char* host = "script.google.com";
const int httpsPort = 443;

//flag for saving data
bool shouldSaveConfig = false;
void configModeCallback (WiFiManager *myWiFiManager);

//
// SHA1 fingerprint of the certificate (get this from the browser)
const char* fingerprint = "54:7B:BB:F6:8D:57:0D:1F:FD:6F:30:37:63:17:24:59:6A:DF:97:FF";


// Prepare the url (without the varying data)
String url = String("/macros/s/") + GScriptID + "/exec?";

HTTPSRedirect* client = nullptr;

DHT dht(DHTPIN, DHTTYPE, 11); // 11 works fine for ESP8266

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void setup()
{
         
  // Enable our serial port.   
  Serial.begin(115200);
  Serial.println();
  
  pinMode(DHTPWR, OUTPUT);  // DHT21 power pin
  digitalWrite(DHTPWR, HIGH);

  pinMode(WIFI_CLR, INPUT_PULLUP);
  if (digitalRead(WIFI_CLR) == LOW) {
    // Hold this button on power up to clear out WiFi settings
    WiFi.disconnect(true);
  }

  // Read params if they are present
  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");

          strcpy(strdeepSleepSecs, json["deepSleepSecs"]);  // Load in deepSleepSecs as a char
          strcpy(sensorID, json["sensorID"]);
          strcpy(GScriptID, json["GScriptID"]);

        } else {
          Serial.println("Failed to load json config");
        }
      }
    } else {
      Serial.println("Warning: Config file does not exist");
    }
  } else {
    Serial.println("Failed to mount FS");
  }
  //end read

  WiFiManagerParameter custom_heading("<br><h3>Sensor Custom Settings</h3>");
  WiFiManagerParameter heading1("<b>Sleep Seconds:</b>");
  WiFiManagerParameter custom_deepSleepSecs("deepSleepSecs", "Sleep Seconds", strdeepSleepSecs, 8);
  WiFiManagerParameter heading2("<br><br><b>Google Sheet Name:</b>");
  WiFiManagerParameter custom_sensorID("sensorID", "Sheet Name", sensorID, 16);
  WiFiManagerParameter heading3("<br><br><b>Google Script ID:</b>");
  WiFiManagerParameter custom_GScriptID("GScriptID", "Script ID", GScriptID, 61);

  // WiFi setup
  WiFiManager wifiManager;
  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  wifiManager.addParameter(&custom_heading);
  wifiManager.addParameter(&heading1);
  wifiManager.addParameter(&custom_deepSleepSecs);
  wifiManager.addParameter(&heading2);
  wifiManager.addParameter(&custom_sensorID);
  wifiManager.addParameter(&heading3);
  wifiManager.addParameter(&custom_GScriptID);

  wifiManager.setTimeout(90);  // 90 seconds to configure this via softAP then we bail
  
  if (!wifiManager.autoConnect(PROJECT_NAME)) {
    Serial.println("Failed to connect to AP - hit timeout");
    delay(3000);

    // do something here like reset or go back to sleep
    digitalWrite(DHTPWR, LOW);  // Turn off DHT21 power
    deepSleepSecs = atoi(strdeepSleepSecs);
    ESP.deepSleep((deepSleepSecs * 1000000)/2); // Go to sleep for half the configured time
  }
  
  //read updated parameters
  strcpy(strdeepSleepSecs, custom_deepSleepSecs.getValue());  // Load in deepSleepSecs as a char
  strcpy(sensorID, custom_sensorID.getValue());
  strcpy(GScriptID, custom_GScriptID.getValue());
  
  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("Saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["deepSleepSecs"] = strdeepSleepSecs;
    json["sensorID"] = sensorID;
    json["GScriptID"] = GScriptID;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("Failed to open config file for writing");
    }

    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
    //end save
  }

  Serial.println("Getting temperature");
  gettemperature(); // read sensor, store values in constants
  analogValue = analogRead(A0);  // Get value from voltage divider
  Serial.print("Analog pin value: ");
  Serial.println(String(analogValue));

  String urlFinal = url + "tab=" + sensorID + "&temp=" + String(temp_f) + "&humidity=" + String(humidity) + "&batt=" + String(analogValue);
  Serial.print("urlFinal: ");
  Serial.println(host + urlFinal);

  Serial.println("Creating new client instance");
  client = new HTTPSRedirect(httpsPort);
  client->setPrintResponseBody(true);
  client->setContentTypeHeader("text/html");

  if (client != nullptr){
    if (!client->connected()){
      Serial.println("Client not connected, connecting...");
      client->connect(host, httpsPort);
      Serial.println("GETting urlFinal");
      if (client->GET(urlFinal, host))  {
        Serial.println("-------------- GET finished ----------------");
      }
    }
  }
  else{
    DPRINTLN("Error creating client object!");
  }
  
  Serial.println("deleting client");
  delete client;
  
  Serial.print("Sleeping for: "); 
  Serial.println(strdeepSleepSecs);
  //delay(100);  // this is just so the serial output above has time to exit the port
  digitalWrite(DHTPWR, LOW);  // Turn off DHT21 power
  deepSleepSecs = atoi(strdeepSleepSecs);
  ESP.deepSleep(deepSleepSecs * 1000000); // Go to sleep

}

void loop() {
  //nada
}


void gettemperature() {

  unsigned long currentMillis = millis();

  humidity = dht.readHumidity();          // Read humidity (percent)
  temp_f = dht.readTemperature(true);     // Read temperature as Fahrenheit
  Serial.println("Temperature read: " + String(temp_f));

  if (isnan(humidity) || isnan(temp_f)) {
    Serial.println("Failed to read from DHT sensor!");
    return;
  }
  
}

void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  //if you used auto generated SSID, print it
  Serial.println(myWiFiManager->getConfigPortalSSID());

}
