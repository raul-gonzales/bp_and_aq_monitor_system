//-----START OF FIREBASE RTDB INCLUDES
#include <WiFi.h>  // wifi library for esp32
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
//-----END OF FIREBASE RTDB INCLUDES

//------START OF AIR SENSORS INCLUDES
#include <HardwareSerial.h>
#include <driver/uart.h>
#include <Wire.h>
#include "SparkFunCCS811.h"
//------END OF AIR SENSORS INCLUDES

//-----START OF FIREBASE RTDB AND WIFI DEFINES
//Wifi Credentials
#define WIFI_SSID "**********"  //SSID of router
#define WIFI_PASSWORD "******"
//Database Credentials
#define DATABASE_URL "https://g12-iot-air-water-monitor-db-default-rtdb.firebaseio.com/"
#define API_KEY "***********************************"
#define DATABASE_SECRET "***********************************"
//UserID
#define USER_EMAIL "*********@*****.***"
#define USER_PASSWORD "******"
//-----END OF FIREBASE RTDB AND WIFI DEFINES

//-----Other defines
#define CCS811_ADDR 0x5B  //Default I2C Address

//----------------------------------------------FIREBASE OBJECTS
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// Variable to save USER UID
String uid;
// Database main path
String dbPath;
//===============================[AIR MONITOR COMPONENT]
//Database parent paths
String dbAirMonitorPath;
String dbFilterIsOnPath;
String dbFilterOnCommandPath;
String dbPoorAirQualityWarningPath;
String dbAutomateSystemCommandPath;
String dbSystemIsAutomatedPath;
String dbco2Path;
String dbtvocPath;
String dbapm10Path;
String dbapm25Path;
String dbaqiPath;

// Database child nodes
String co2Path = "/Data-CO2";
String tvocPath = "/Data-TVOC";
String apm25Path = "/Data-Apm2-5";
String apm10Path = "/Data-Apm10";
String aqiPath = "/Data-AQI";
String focPath = "/Command-Filter-On";
String ascPath = "/Command-Automate-System";
String fioPath = "/Is-Filter-On";
String siaPath = "/Is-System-Automated";
String paqwPath = "/Warning-Poor-Air-Quality";

//needed for millis function for read/write at specified interval in a non blocking way
unsigned long aqSendDataPrevMillis = 0;
unsigned long aqReadMillis = 0;
//for esp32 timing
unsigned long espMillis = 0;
bool signupOK = false;

//Other variables
CCS811 mySensor(CCS811_ADDR);
bool automateSystemCommand = false;
bool systemIsAutomated = false;
bool filterOnCommand = false;
bool filterIsOn = false;
bool poorAirQualityWarning = false;
bool lastPoorAirQualityWarning = poorAirQualityWarning;
bool paqWarn = false;

const int TOL = 130;
int aqi = 1;
float apm10 = 1;
float apm25 = 1;
float tvoc = 1;
float co2 = 1;
bool systemReset = true;
unsigned long warnLockMillis = 0;
unsigned long fanOnMillis = 0;
unsigned long delayMillis = 0;
bool fanOnTimer = false;
bool warnLocked = true;
bool poorAirQualityChanged = false;
bool lastPoorAirQualityChanged = poorAirQualityChanged;

//multistream variables
FirebaseData multiStream;
String eventListenerNodePaths[3] = {"/Command-Filter-On", "/Command-Automate-System"};
volatile bool ascDataChanged = false;
volatile bool focDataChanged = false;

//----------------------------------------------setup()
void setup()
{
  //==========================Start serial communication for debugging purposes
  Serial.begin(115200);
  //==========================Initialize I2C Hardware
  Wire.begin();  //Inialize I2C Hardware default I2C pins are gpio 22 and 21
  if (mySensor.begin() == false)
  {
    Serial.print("CCS811 error. Please check wiring. Freezing...");
    while (1)
      ;
  }
  //==========================Connect to WiFi
  initWifi();
  //==========================Configure using database credentials
  // Configure api key
  config.api_key = API_KEY;
  // Configure RTDB url
  config.database_url = DATABASE_URL;

  Firebase.reconnectWiFi(true);
  fbdo.setResponseSize(4096);
  //-------------------------------------------SIGN IN WITH USERNAME AND PASSWORD----------------------------------------------------
  // Configure user sign in credentials
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  signupOK = true;
  //---------------------------------------------------------------------------------------------------------------------------------

  // Configure callback function for long running generation task
  config.token_status_callback = tokenStatusCallback;
  // Configure max retry of token generation
  config.max_token_generation_retry = 5;

  // Initialize the library with the Firebase authen and config
  Firebase.begin(&config, &auth);

  // Obtain user UID
  Serial.println("Obtaining User UID");
  while ((auth.token.uid) == "") {
    Serial.print('.');
    delay(1000);
  }
  Serial.begin(9600);
  Serial1.begin(9600, SERIAL_8N1, 9, 10);

  uid = auth.token.uid.c_str();
  Serial.print("User UID: ");
  Serial.println(uid);

  // Update databaseMainPath
  dbPath = "/User-Data/" + uid;
  // Update air quality database path
  dbAirMonitorPath = dbPath + "/System-Air-Monitor";

  // beign multistream
  if (!Firebase.RTDB.beginMultiPathStream(&multiStream, dbAirMonitorPath))
    Serial.printf("Multi stream begin error, %s\n\n", multiStream.errorReason().c_str());

  Firebase.RTDB.setMultiPathStreamCallback(&multiStream, streamCallback, streamTimeoutCallback);

}

struct pms5003data
{
  uint16_t framelen;
  uint16_t pm10_standard, pm25_standard, pm100_standard;
  uint16_t pm10_env, pm25_env, pm100_env;
  uint16_t particles_03um, particles_05um, particles_10um, particles_25um, particles_50um, particles_100um;
  uint16_t unused;
  uint16_t checksum;
};

struct pms5003data data;

boolean readData(Stream *s)
{
  if (!s->available())
  { //get the number of bytes (characters) available for reading from the serial port.
    //This is data that’s already arrived and stored in the serial receive buffer (which holds 64 bytes).
    return false;
  }

  // Read a byte at a time until we get to the '0x42' start-byte
  if (s->peek() != 0x42)
  {
    s->read();
    return false;
  }

  // Now read all 32 bytes
  if (s->available() < 32)
  {
    return false;
  }

  uint8_t buffer[32];
  uint16_t sum = 0;
  s->readBytes(buffer, 32);

  // get checksum ready
  for (uint8_t i = 0; i < 30; i++)
  {
    sum += buffer[i];
  }
  // The data comes in endian'd, this solves it so it works on all platforms
  uint16_t buffer_u16[15];
  for (uint8_t i = 0; i < 15; i++)
  {
    buffer_u16[i] = buffer[2 + i * 2 + 1];
    buffer_u16[i] += (buffer[2 + i * 2] << 8);
  }

  // put it into a nice struct :)
  memcpy((void *)&data, (void *)buffer_u16, 30);

  if (sum != data.checksum)
  {
    Serial.println("Checksum failure");
    return false;
  }
  // success!
  return true;
}

int Calc_AQI(uint16_t value)
{
  int AQI = -1;
  if (value >= 0 && value < 12)
  {
    AQI = ((value - 0) * (50 - 0) / (12 - 0)) + 0;
  }
  if (value >= 12.1 && value < 35.4)
  {
    AQI = ((value - 12.1) * (100 - 51) / (35.4 - 12.1)) + 51;
  }
  if (value >= 35.5 && value < 55.4)
  {
    AQI = ((value - 35.5) * (150 - 101) / (55.4 - 35.5)) + 101;
  }
  if (value >= 55.5 && value < 150.4)
  {
    AQI = ((value - 55.5) * (200 - 151) / (150.4 - 55.5)) + 151;
  }
  if (value >= 150.5 && value < 250.4)
  {
    AQI = ((value - 150.5) * (300 - 201) / (250.4 - 150.5)) + 201;
  }
  if (value >= 250.5 && value < 350.4)
  {
    AQI = ((value - 250.5) * (400 - 301) / (350.4 - 250.5)) + 301;
  }
  if (value >= 350.5 && value < 500.4)
  {
    AQI = ((value - 350.5) * (500 - 401) / (500.4 - 350.5)) + 401;
  }
  return AQI;
}

//----------------------------------------------loop()
void loop()
{
  //===================================[AIR QUALITY MONITOR]=====================================
  // sensor read every 1 second
  if ((millis() - aqReadMillis > 1 * 1000) && readData(&Serial1))
  {
    aqReadMillis = millis();
    // read sensor code here ↓
    apm25 = (float)data.pm25_env;
    apm10 = (float)data.pm100_env;
    aqi = Calc_AQI(data.pm25_env);
    Serial.println("-------------------A--------------------");
    Serial.println("Concentration Units (ug/m3)");  //environmental
    Serial.printf("PM 1.0: %f\nPM 2.5: %f\nPM 10: %f\n", (float)data.pm10_env, (float)data.pm25_env, (float)data.pm100_env);
    Serial.println("------------------A---------------------");
    Serial.printf("Number of particles > 0.3um / 0.1L air: %f\n", (float)data.particles_03um);
    Serial.printf("Number of particles > 0.5um / 0.1L air: %f\n", (float)data.particles_05um);
    Serial.printf("Number of particles > 1.0um / 0.1L air: %f\n", (float)data.particles_10um);
    Serial.printf("Number of particles > 2.5um / 0.1L air: %f\n", (float)data.particles_25um);
    Serial.printf("Number of particles > 5.0um / 0.1L air: %f\n", (float)data.particles_50um);
    Serial.printf("Number of particles > 10.0 um / 0.1L air: %f\n", (float)data.particles_100um);
    Serial.println("---------------------------------------");
    Serial.printf("AQI VALUE: %d\n", aqi);

    if (mySensor.dataAvailable())
    {
      mySensor.readAlgorithmResults();
      co2 = mySensor.getCO2();
      tvoc = mySensor.getTVOC();
      Serial.printf("CO2[ %d ] tVOC[ %d ]\n", co2, tvoc);
    }
  }

  //test the aq change for setting the boolean value in database
  //set air quality thresholds
  if (aqi > TOL)
  {
    poorAirQualityWarning = true;
    poorAirQualityChanged = true;
  }
  else
  {
    poorAirQualityWarning = false;
    poorAirQualityChanged = false;
  }
  // Check for pipe burst state change and start timer
  if (poorAirQualityChanged != lastPoorAirQualityChanged)
  {
    lastPoorAirQualityChanged = poorAirQualityChanged;
    warnLockMillis = millis();
  }

  //___________________________________AIR QUALITY AND PARTICULATE MATTER DATA SENDING
  // Send air quality and apm readings to database every 1 second
  if (Firebase.ready() && signupOK)
  {
    //_________________________________INITIALIZE PATHS ON RESET
    // Poor-Air-Quality-Warning | Filter-Is-On | Filter-On-Command | Automate-System-Command | System-Is-Automated
    if (systemReset)
    { //initialize paths
      // Update paths
      dbPoorAirQualityWarningPath = dbAirMonitorPath + paqwPath;
      dbFilterIsOnPath = dbAirMonitorPath + fioPath;
      dbFilterOnCommandPath = dbAirMonitorPath + focPath;
      dbAutomateSystemCommandPath = dbAirMonitorPath + ascPath;
      dbSystemIsAutomatedPath = dbAirMonitorPath + siaPath;
      dbco2Path = dbAirMonitorPath + co2Path;
      dbtvocPath = dbAirMonitorPath + tvocPath;
      dbapm10Path = dbAirMonitorPath + apm10Path;
      dbapm25Path = dbAirMonitorPath + apm25Path;
      dbaqiPath = dbAirMonitorPath + aqiPath;
      Serial.printf("Checking fio node...%s\n", Firebase.RTDB.getBool(&fbdo, dbFilterIsOnPath.c_str(), &filterIsOn) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Checking foc node...%s\n", Firebase.RTDB.getBool(&fbdo, dbFilterOnCommandPath.c_str(), &filterOnCommand) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Checking asc node...%s\n", Firebase.RTDB.getBool(&fbdo, dbAutomateSystemCommandPath.c_str(), &automateSystemCommand) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Checking sia node...%s\n", Firebase.RTDB.getBool(&fbdo, dbSystemIsAutomatedPath.c_str(), &systemIsAutomated) ? "ok" : fbdo.errorReason().c_str());
      // Set data nodes with values read from db
      Serial.printf("Initializing paqw node...%s\n", Firebase.RTDB.setBool(&fbdo, dbPoorAirQualityWarningPath.c_str(), poorAirQualityWarning) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Initializing fio node...%s\n", Firebase.RTDB.setBool(&fbdo, dbFilterIsOnPath.c_str(), filterIsOn) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Initializing foc node...%s\n", Firebase.RTDB.setBool(&fbdo, dbFilterOnCommandPath.c_str(), filterOnCommand) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Initializing asc node...%s\n", Firebase.RTDB.setBool(&fbdo, dbAutomateSystemCommandPath.c_str(), automateSystemCommand) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Initializing sia node...%s\n", Firebase.RTDB.setBool(&fbdo, dbSystemIsAutomatedPath.c_str(), systemIsAutomated) ? "ok" : fbdo.errorReason().c_str());
      // finished reboot
      systemReset = false;
    }
    //send air quality data every 1 secs to the database
    if ((millis() - aqSendDataPrevMillis > 1 * 1000))
    {
      aqSendDataPrevMillis = millis();// update prev timer with current time
      //update path for air quality readings
      Serial.printf("Updating AQI... %s\n", Firebase.RTDB.setInt(&fbdo, dbaqiPath.c_str(), aqi) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Updating tvoc... %s\n", Firebase.RTDB.setFloat(&fbdo, dbtvocPath.c_str(), tvoc) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Updating CO2... %s\n", Firebase.RTDB.setFloat(&fbdo, dbco2Path.c_str(), co2) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Updating Apm-1-0... %s\n", Firebase.RTDB.setFloat(&fbdo, dbapm10Path.c_str(), apm10) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Updating Apm-2-5... %s\n", Firebase.RTDB.setFloat(&fbdo, dbapm25Path.c_str(), apm25) ? "ok" : fbdo.errorReason().c_str());
    }
    //___________________________________POOR AIR QUALITY WARNINGS
    //------------------------------------------Warning delays
    // Check if poorAirQualityWarning changed to true, send after delayed amount of time
    if ((poorAirQualityWarning != lastPoorAirQualityWarning) && poorAirQualityWarning)
    {
      if (warnLocked)
        // Allow some time to pass before unlocking warnLock to let air quality settle first
      {
        if (millis() - warnLockMillis > 5 * 1000)
        {
          warnLockMillis = millis();
          warnLocked = false;
          Serial.println("Unlocking warn");
        }
      }
      // if warn is not locked, wait specified time before changing state
      if (!warnLocked)
      {
        lastPoorAirQualityWarning = poorAirQualityWarning;
        // Update poor air quality warning path
        dbPoorAirQualityWarningPath = dbAirMonitorPath + paqwPath;
        Serial.printf("State change on warning, setting node to new state...%s\n", Firebase.RTDB.setBool(&fbdo, dbPoorAirQualityWarningPath.c_str(), poorAirQualityWarning) ? "ok" : fbdo.errorReason().c_str());
        warnLocked = true;
        if (poorAirQualityWarning == true)
        {
          paqWarn = true;
        }
        else if (poorAirQualityWarning == false)
        {
          paqWarn = false;
        }
      }
    }
    // Check if poorAirQuality warning changed to false
    else if ((poorAirQualityWarning != lastPoorAirQualityWarning) && !poorAirQualityWarning)
    {
      if (warnLocked)
        // Allow some time to pass before unlocking warnLock to let air quality settle first
      { // change this ↓↓ value to modify time to keep fan on upon reaching safe aqi levels
        if (millis() - warnLockMillis > 20 * 1000)// ## * 1000 = ## seconds
        {
          warnLockMillis = millis();
          warnLocked = false;
          Serial.println("Unlocking warn");
        }
      }
      // if warn is not locked, wait specified time before changing state
      if (!warnLocked)
      {
        lastPoorAirQualityWarning = poorAirQualityWarning;
        // Update poor air quality warning path
        dbPoorAirQualityWarningPath = dbAirMonitorPath + paqwPath;
        Serial.printf("State change on warning, setting node to new state...%s\n", Firebase.RTDB.setBool(&fbdo, dbPoorAirQualityWarningPath.c_str(), poorAirQualityWarning) ? "ok" : fbdo.errorReason().c_str());
        warnLocked = true;
        if (poorAirQualityWarning == true)
        {
          paqWarn = true;
        }
        else if (poorAirQualityWarning == false)
        {
          paqWarn = false;
        }
      }
    }
    //___________________________________EVENT LISTENER AND ACTION
    // listen for filter on commands
    if (focDataChanged)
    { // do things here when Open-Valve-Command node changes
      focDataChanged = false;
      Serial.print("Received Filter control command: ");
      Serial.println(multiStream.to<bool>() == 1 ? "ON" : "OFF");
      if (multiStream.to<bool>() == true)
      {
        filterIsOn = true;
        // Successfully changed filter fan state, updating database node /Filter-Is-On
        Serial.printf("Updating fio node...%s\n", Firebase.RTDB.setBool(&fbdo, dbFilterIsOnPath.c_str(), filterIsOn) ? "ok" : fbdo.errorReason().c_str());
      }
      else if (multiStream.to<bool>() == false)
      {
        filterIsOn = false;
        // Successfully changed filter fan state, updating database node /Filter-Is-On
        Serial.printf("Updating fio node...%s\n", Firebase.RTDB.setBool(&fbdo, dbFilterIsOnPath.c_str(), filterIsOn) ? "ok" : fbdo.errorReason().c_str());
      }
    }
    //listen for automate system commands
    if (ascDataChanged)
    { // do things here when Automate-System node changes
      ascDataChanged = false;
      Serial.print("Received Automation command: ");
      Serial.println(multiStream.to<bool>() == 1 ? "ON" : "OFF");
      if (multiStream.to<bool>() == true)
      {
        systemIsAutomated = true;
        // Successfully changed automation state, updating database node /System-Is-Automated
        Serial.printf("Updating sia node...%s\n", Firebase.RTDB.setBool(&fbdo, dbSystemIsAutomatedPath.c_str(), systemIsAutomated) ? "ok" : fbdo.errorReason().c_str());
      }
      else if (multiStream.to<bool>() == false)
      {
        systemIsAutomated = false;
        // Successfully changed automation state, updating database node /System-Is-Automated
        Serial.printf("Updating sia node...%s\n", Firebase.RTDB.setBool(&fbdo, dbSystemIsAutomatedPath.c_str(), systemIsAutomated) ? "ok" : fbdo.errorReason().c_str());
      }
    }
  }
  //AUTOMATED-SYSTEM========================================================================
  if (systemIsAutomated && (millis() - delayMillis > 1000))
  {
    delayMillis = millis();
    // Close valve if any warning is set to true, otherwise leave it closed
    if (paqWarn)
    {
      Serial.println("Fan on");
      filterIsOn = true;
      //<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<INSERT FAN ON CODE HERE
      Serial.printf("Updating node...%s\n", Firebase.RTDB.setBool(&fbdo, dbFilterIsOnPath.c_str(), filterIsOn) ? "ok" : fbdo.errorReason().c_str());
    }
    else if (!paqWarn)
    {
      Serial.println("Fan off");
      filterIsOn = false;
      //<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<INSERT FAN OFF CODE HERE
      Serial.printf("Updating node...%s\n", Firebase.RTDB.setBool(&fbdo, dbFilterIsOnPath.c_str(), filterIsOn) ? "ok" : fbdo.errorReason().c_str());
    }
  }
  else if (!systemIsAutomated && (millis() - delayMillis > 1000))
  {
    delayMillis = millis();
    if (filterIsOn)
    {
      Serial.println("Fan on");
      //<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<INSERT FAN ON CODE HERE
    }
    else if (!filterIsOn)
    {
      Serial.println("Fan off");
      //<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<INSERT FAN OFF CODE HERE
    }
  }

}//loop()

//___________________________________________________________________________________________
//=======================================[FUNCTIONS]=========================================
//-----------------------------------------------------------------------------initWifi()
void initWifi()
{
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nConnected, IP address: ");
  Serial.println(WiFi.localIP());
  Serial.println();
}//initWifi

void streamCallback(MultiPathStream stream)
{
  size_t numChild = sizeof(eventListenerNodePaths) / sizeof(eventListenerNodePaths[0]);
  for (size_t i = 0; i < numChild; i++)
  {
    if (stream.get(eventListenerNodePaths[i]))
    {
      Serial.printf("path: %s, event: %s, type: %s, value: %s%s", stream.dataPath.c_str(), stream.eventType.c_str(), stream.type.c_str(), stream.value.c_str(), i < numChild - 1 ? "\n" : "");
      if (!(strcmp(stream.dataPath.c_str(), "/Command-Filter-On")))
      {
        focDataChanged = true;
      }
      if (!(strcmp(stream.dataPath.c_str(), "/Command-Automate-System")))
      {
        ascDataChanged = true;
      }
    }
  }
  Serial.printf("Received stream payload size: %d (Max. %d)\n\n", stream.payloadLength(), stream.maxPayloadLength());
}

void streamTimeoutCallback(bool timeout)
{
  if (timeout)
    Serial.println("stream timed out, resuming...\n");

  if (!multiStream.httpConnected())
    Serial.printf("error code: %d, reason: %s\n\n", multiStream.httpCode(), multiStream.errorReason().c_str());
}
