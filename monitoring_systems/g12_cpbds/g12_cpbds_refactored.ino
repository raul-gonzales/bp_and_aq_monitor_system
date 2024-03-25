//----------------------------------------------INCLUDES
#include <WiFi.h> // wifi library for esp32
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <limits.h>
#include <HL-52S.h>

//----------------------------------------------DEFINES AND VARIABLES
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
// Data wire is plugged into port 4 on the Arduino
#define ONE_WIRE_BUS 4
#define RELAY_PIN 2
// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(ONE_WIRE_BUS);
// Pass our oneWire reference to Dallas Temperature.
DallasTemperature sensors(&oneWire);
// Setup a relay instance to control solenoid valve through the relay
Relay solenoidRelay(RELAY_PIN);
//----------------------------------------------FIREBASE OBJECTS
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// Variable to save USER UID
String uid;
// Database main path
String dbPath;
//===============================[WATER MONITOR COMPONENT]
// Database parent paths
String dbColdTempPath, dbWaterMonitorPath;
String dbColdPipeBurstWarningPath, dbHotPipeBurstWarningPath;
String dbOpenValveCommandPath, dbAutomateSystemCommandPath, dbAtHomeCommandPath;
String dbSystemIsAutomatedPath, dbValveIsOpenPath, dbIsAtHomePath;

// Database child nodes
String cTempPath = "/Data-Cold-Temp";
String cpbwPath = "/Warning-Cold-Pipe-Burst";
String hpbwPath = "/Warning-Hot-Pipe-Burst";
String ovcPath = "/Command-Open-Valve";
String ascPath = "/Command-Automate-System";
String ahcPath = "/Command-At-Home";
String vioPath = "/Is-Valve-Open";
String siaPath = "/Is-System-Automated";
String iahPath = "/Is-At-Home";

// needed for millis function read/write at specified interval in a non blocking way
unsigned long tempSendDataPrevMillis = 0;
unsigned long tempReadMillis = 0;
// for esp32 timing
unsigned long espMillis = 0;
unsigned long delayMillis = 0;
// boolean for signed in
bool signupOK = false;
// hardcoded device addresses for sensors
uint8_t roomTemperature[8] = {0x28, 0x8D, 0xFB, 0xCA, 0x5E, 0x14, 0x01, 0x4C};
uint8_t pipeTemperature[8] = {0x28, 0x95, 0x1C, 0xCD, 0x5E, 0x14, 0x01, 0xBB};

// Other variables
bool automateSystemCommand = false;
bool systemIsAutomated = false;
bool openValveCommand = true;
bool valveIsOpen = true;
bool isAtHome = true;
bool atHomeCommand = true;
bool coldPipeBurstWarning = false;
bool hotPipeBurstWarning = false;
bool lastColdPipeBurstWarning = coldPipeBurstWarning;
bool lastHotPipeBurstWarning = hotPipeBurstWarning;
bool coldWarn = false;

const float TOL = 5.0; // how much hotter the pipe temp needs to be
float coldTemp = 0;
float roomTemp = 0;
int count = 0;
bool systemReset = true;
unsigned long warnLockMillis = 0;
unsigned long atHomeWarnLockMillis = 0;
bool warnLocked = true;
bool pipeBurstChanged = false;
bool lastPipeBurstChanged = pipeBurstChanged;
bool tempSensorsConnected = true;

//multistream variables
FirebaseData multiStream;
String eventListenerNodePaths[4] = {"/Command-Open-Valve", "/Command-Automate-System", "/Command-At-Home", "/Warning-Hot-Pipe-Burst"};
volatile bool ascDataChanged = false;
volatile bool ovcDataChanged = false;
volatile bool ahcDataChanged = false;
volatile bool hpbwDataChanged = false;

//----------------------------------------------setup()
void setup()
{
  //==========================Start serial communication for debugging purposes
  Serial.begin(115200);
  //===========================Start up the sensors library
  sensors.begin();
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
  while ((auth.token.uid) == "")
  {
    Serial.print('.');
    delay(1000);
  }
  uid = auth.token.uid.c_str();
  Serial.print("User UID: ");
  Serial.println(uid);

  // Update databaseMainPath
  dbPath = "/User-Data/" + uid;
  // Update pipe temperature database path
  dbWaterMonitorPath = dbPath + "/System-Water-Monitor";

  // beign multistream
  if (!Firebase.RTDB.beginMultiPathStream(&multiStream, dbWaterMonitorPath))
    Serial.printf("Multi stream begin error, %s\n\n", multiStream.errorReason().c_str());

  Firebase.RTDB.setMultiPathStreamCallback(&multiStream, streamCallback, streamTimeoutCallback);

}

//----------------------------------------------loop()
void loop()
{
  //===================================[WATER SUPPLY LINE]=====================================
  //___________________________________TEMPERATURE DATA READING
  // sensor read every 1 second
  if ((tempReadMillis == 0) || (millis() - tempReadMillis > 1 * 1000))
  {
    tempReadMillis = millis();

    // attempt to read sensor temperatures
    sensors.requestTemperatures();
    coldTemp = sensors.getTempC(pipeTemperature);
    roomTemp = sensors.getTempC(roomTemperature);

    // check if devices are connected then output temp data on the serial monitor
    if ((coldTemp != DEVICE_DISCONNECTED_C) && (roomTemp != DEVICE_DISCONNECTED_C))
    {
      tempSensorsConnected = true;
      Serial.printf("Cold Pipe Temperature is: %f\n", coldTemp);
      Serial.printf("Room Temperature is: %f", roomTemp);
    }
    else
    {
      Serial.println("Error: Devices are disconnected");
      tempSensorsConnected = false;
    }
  }

  // pipe burst warning threshold
  if (abs(roomTemp - coldTemp) > TOL)
  {
    coldPipeBurstWarning = true;
    pipeBurstChanged = true;
  }
  else
  {
    coldPipeBurstWarning = false;
    pipeBurstChanged = false;
  }
  // Check for pipe burst state change and start timer
  if (pipeBurstChanged != lastPipeBurstChanged)
  {
    lastPipeBurstChanged = pipeBurstChanged;
    warnLockMillis = millis();
  }

  //___________________________________TEMPERATURE DATA SENDING
  // Send hot/cold water pipe temp readings with timestamp to database every 5 seconds
  if (Firebase.ready() && signupOK)
  {
    //_________________________________INITIALIZE PATHS ON RESET
    // Pipe-Burst-Warning | Valve-Is-Open | Open-Valve-Command | Automate-System-Command | System-Is-Automated
    if (systemReset)
    { // initialize paths
      // Update paths
      dbColdPipeBurstWarningPath = dbWaterMonitorPath + cpbwPath;
      dbValveIsOpenPath = dbWaterMonitorPath + vioPath;
      dbOpenValveCommandPath = dbWaterMonitorPath + ovcPath;
      dbAutomateSystemCommandPath = dbWaterMonitorPath + ascPath;
      dbSystemIsAutomatedPath = dbWaterMonitorPath + siaPath;
      dbAtHomeCommandPath = dbWaterMonitorPath + ahcPath;
      dbIsAtHomePath = dbWaterMonitorPath + iahPath;
      dbColdTempPath = dbWaterMonitorPath + cTempPath;
      dbHotPipeBurstWarningPath = dbWaterMonitorPath + hpbwPath;
      // Get current values from database
      Serial.printf("Checking vio node...%s\n", Firebase.RTDB.getBool(&fbdo, dbValveIsOpenPath.c_str(), &valveIsOpen) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Checking ovc node...%s\n", Firebase.RTDB.getBool(&fbdo, dbOpenValveCommandPath.c_str(), &openValveCommand) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Checking asc node...%s\n", Firebase.RTDB.getBool(&fbdo, dbAutomateSystemCommandPath.c_str(), &automateSystemCommand) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Checking sia node...%s\n", Firebase.RTDB.getBool(&fbdo, dbSystemIsAutomatedPath.c_str(), &systemIsAutomated) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Checking iah node...%s\n", Firebase.RTDB.getBool(&fbdo, dbIsAtHomePath.c_str(), &isAtHome) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Checking ahc node...%s\n", Firebase.RTDB.getBool(&fbdo, dbAtHomeCommandPath.c_str(), &atHomeCommand) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Checking hpbw node...%s\n", Firebase.RTDB.getBool(&fbdo, dbHotPipeBurstWarningPath.c_str(), &hotPipeBurstWarning) ? "ok" : fbdo.errorReason().c_str());
      // Set data nodes with values read from db
      Serial.printf("Setting pbw node...%s\n", Firebase.RTDB.setBool(&fbdo, dbColdPipeBurstWarningPath.c_str(), coldPipeBurstWarning) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Setting vio node...%s\n", Firebase.RTDB.setBool(&fbdo, dbValveIsOpenPath.c_str(), valveIsOpen) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Setting ovc node...%s\n", Firebase.RTDB.setBool(&fbdo, dbOpenValveCommandPath.c_str(), openValveCommand) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Setting asc node...%s\n", Firebase.RTDB.setBool(&fbdo, dbAutomateSystemCommandPath.c_str(), automateSystemCommand) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Setting sia node...%s\n", Firebase.RTDB.setBool(&fbdo, dbSystemIsAutomatedPath.c_str(), systemIsAutomated) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Setting iah node...%s\n", Firebase.RTDB.setBool(&fbdo, dbIsAtHomePath.c_str(), isAtHome) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Setting ahc node...%s\n", Firebase.RTDB.setBool(&fbdo, dbAtHomeCommandPath.c_str(), atHomeCommand) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Setting cold pipe temp node... %s\n", Firebase.RTDB.setFloat(&fbdo, dbColdTempPath.c_str(), coldTemp) ? "ok" : fbdo.errorReason().c_str());
      // finished reboot initializes
      systemReset = false;
    }
    // send temp data every 1 secs to the database
    if (millis() - tempSendDataPrevMillis > 1 * 1000 && tempSensorsConnected)
    {
      tempSendDataPrevMillis = millis(); // update prev timer with current time
      // update path for water pipes readings
      Serial.printf("Updating Data-Cold-Temp... %s\n", Firebase.RTDB.setFloat(&fbdo, dbColdTempPath.c_str(), coldTemp) ? "ok" : fbdo.errorReason().c_str());
    }
    //___________________________________PIPE BURST WARNINGS
    //------------------------------------------AWAY FROM HOME Warning delays
    // Check if not at home is true, then send warning after 5 seconds stable value
    if (coldPipeBurstWarning != lastColdPipeBurstWarning && !isAtHome)
    {
      if (warnLocked)
        // Allow some time to pass before unlocking warnLock to let temperature settle first
      {
        if (millis() - warnLockMillis > 5 * 1000)
        {
          warnLockMillis = millis();
          warnLocked = false;
          Serial.println("Unlocking warn");
        }
      }
      // Check if warn is not locked then update node
      if (!warnLocked)
      {
        lastColdPipeBurstWarning = coldPipeBurstWarning;
        // Update pipe burst warning path
        dbColdPipeBurstWarningPath = dbWaterMonitorPath + cpbwPath;
        Serial.printf("State change on warning, setting node to new state...%s\n", Firebase.RTDB.setBool(&fbdo, dbColdPipeBurstWarningPath.c_str(), coldPipeBurstWarning) ? "ok" : fbdo.errorReason().c_str());
        warnLocked = true;
        if (coldPipeBurstWarning == true)
        {
          coldWarn = true;
        }
        else if (coldPipeBurstWarning == false)
        {
          coldWarn = false;
        }
      }
    }
    //------------------------------------------INSIDE HOME Warning delays
    // Check if at home is true and coldPipeBurstWarning changed to true, send after delayed amount of time
    if ((coldPipeBurstWarning != lastColdPipeBurstWarning) && isAtHome && coldPipeBurstWarning)
    {
      if (warnLocked)
        // Allow some time to pass before unlocking warnLock to let temperature settle first
      {
        if (millis() - warnLockMillis > 30 * 1000)
        {
          warnLockMillis = millis();
          warnLocked = false;
          Serial.println("Unlocking warn");
        }
      }
      // if warn is not locked, wait specified time before changing state
      if (!warnLocked)
      {
        lastColdPipeBurstWarning = coldPipeBurstWarning;
        // Update pipe burst warning path
        dbColdPipeBurstWarningPath = dbWaterMonitorPath + cpbwPath;
        Serial.printf("State change on warning, setting node to new state...%s\n", Firebase.RTDB.setBool(&fbdo, dbColdPipeBurstWarningPath.c_str(), coldPipeBurstWarning) ? "ok" : fbdo.errorReason().c_str());
        warnLocked = true;

        if (coldPipeBurstWarning == true)
        {
          coldWarn = true;
        }
        else if (coldPipeBurstWarning == false)
        {
          coldWarn = false;
        }
      }
    }
    // Check if at home is true and coldPipeBurstWarning changed to false, send after 5 seconds stable value
    else if ((coldPipeBurstWarning != lastColdPipeBurstWarning) && isAtHome && !coldPipeBurstWarning)
    {
      if (warnLocked)
        // Allow some time to pass before unlocking warnLock to let temperature settle first
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
        lastColdPipeBurstWarning = coldPipeBurstWarning;
        // Update pipe burst warning path
        dbColdPipeBurstWarningPath = dbWaterMonitorPath + cpbwPath;
        Serial.printf("State change on warning, setting node to new state...%s\n", Firebase.RTDB.setBool(&fbdo, dbColdPipeBurstWarningPath.c_str(), coldPipeBurstWarning) ? "ok" : fbdo.errorReason().c_str());
        warnLocked = true;

        if (coldPipeBurstWarning == true)
        {
          coldWarn = true;
        }
        else if (coldPipeBurstWarning == false)
        {
          coldWarn = false;
        }
      }
    }
    //___________________________________EVENT LISTENER AND STATE CHANGE
    // listen for open valve commands, turn off automation if any command here is received
    if (ovcDataChanged)
    { // do things here when Open-Valve-Command node changes
      ovcDataChanged = false;
      Serial.print("Received Valve control command: ");
      Serial.println(multiStream.to<bool>() == 1 ? "OPEN" : "CLOSE");
      if (multiStream.to<bool>() == true)
      {
        valveIsOpen = true;
        // Successfully changed valve state, updating database node /Valve-Is-Open
        Serial.printf("Updating vio node...%s\n", Firebase.RTDB.setBool(&fbdo, dbValveIsOpenPath.c_str(), valveIsOpen) ? "ok" : fbdo.errorReason().c_str());
      }
      else if (multiStream.to<bool>() == false)
      {
        valveIsOpen = false;
        // Successfully changed valve state, updating database node /Valve-Is-Open
        Serial.printf("Updating vio node...%s\n", Firebase.RTDB.setBool(&fbdo, dbValveIsOpenPath.c_str(), valveIsOpen) ? "ok" : fbdo.errorReason().c_str());
      }
    }
    //listen for at home commands
    if (ahcDataChanged)
    { // do things here when at-home-commmand node changes
      ahcDataChanged = false;
      Serial.print("Received Home state command: ");
      Serial.println(multiStream.to<bool>() == 1 ? "HOME" : "AWAY");
      if (multiStream.to<bool>() == true)
      {
        isAtHome = true;
        // Successfully changed home state, updating database node /Is-At-Home
        Serial.printf("Updating iah node...%s\n", Firebase.RTDB.setBool(&fbdo, dbIsAtHomePath.c_str(), isAtHome) ? "ok" : fbdo.errorReason().c_str());
      }
      else if (multiStream.to<bool>() == false)
      {
        isAtHome = false;
        // Successfully changed home state, updating database node /Is-At-Home
        Serial.printf("Updating iah node...%s\n", Firebase.RTDB.setBool(&fbdo, dbIsAtHomePath.c_str(), isAtHome) ? "ok" : fbdo.errorReason().c_str());
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
    //listen for hot pipeburstwarning state change
    if (hpbwDataChanged)
    { // do things here when Automate-System node changes
      hpbwDataChanged = false;
      Serial.print("Received hot pipeburst warning: ");
      Serial.println(multiStream.to<bool>() == 1 ? "True" : "False");
      if (multiStream.to<bool>() == true)
      {
        hotPipeBurstWarning = true;
      }
      else if (multiStream.to<bool>() == false)
      {
        hotPipeBurstWarning = false;
      }
    }
  }

  //AUTOMATED-SYSTEM========================================================================
  if (!systemIsAutomated && (millis()- delayMillis > 1000))
  {
    delayMillis = millis();
    if (valveIsOpen)
    {
      Serial.println("Valve open");
      solenoidRelay.on();
    }
    else if (!valveIsOpen)
    {
      Serial.println("Valve closed");
      solenoidRelay.off();
    }
  }
  else if (systemIsAutomated && (millis()- delayMillis > 1000))
  {
    delayMillis = millis();
    // Close valve if any warning is set to true, otherwise leave it closed
    if (hotPipeBurstWarning || coldWarn)
    {
      Serial.println("Valve closed");
      valveIsOpen = false;
      solenoidRelay.off();
      Serial.printf("Updating node...%s\n", Firebase.RTDB.setBool(&fbdo, dbValveIsOpenPath.c_str(), valveIsOpen) ? "ok" : fbdo.errorReason().c_str());
    }
    else if (!hotPipeBurstWarning && !coldWarn)
    {
      Serial.println("Valve opened");
      valveIsOpen = true;
      solenoidRelay.on();
      Serial.printf("Updating node...%s\n", Firebase.RTDB.setBool(&fbdo, dbValveIsOpenPath.c_str(), valveIsOpen) ? "ok" : fbdo.errorReason().c_str());
    }
  }

  // Resync data every 5 minutes
  if (millis() - espMillis > 300 * 1000 )
  {
    espMillis = millis();
    systemReset = true;
    Serial.println("Re-syncing with database.......");
  }
} // loop()

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
} // initWifi

void streamCallback(MultiPathStream stream)
{
  size_t numChild = sizeof(eventListenerNodePaths) / sizeof(eventListenerNodePaths[0]);
  for (size_t i = 0; i < numChild; i++)
  {
    if (stream.get(eventListenerNodePaths[i]))
    {
      Serial.printf("path: %s, event: %s, type: %s, value: %s%s", stream.dataPath.c_str(), stream.eventType.c_str(), stream.type.c_str(), stream.value.c_str(), i < numChild - 1 ? "\n" : "");
      if (!(strcmp(stream.dataPath.c_str(), "/Command-At-Home")))
      {
        ahcDataChanged = true;
      }
      if (!(strcmp(stream.dataPath.c_str(), "/Command-Open-Valve")))
      {
        ovcDataChanged = true;
      }
      if (!(strcmp(stream.dataPath.c_str(), "/Command-Automate-System")))
      {
        ascDataChanged = true;
      }
      if (!(strcmp(stream.dataPath.c_str(), "/Warning-Hot-Pipe-Burst")))
      {
        hpbwDataChanged = true;
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
