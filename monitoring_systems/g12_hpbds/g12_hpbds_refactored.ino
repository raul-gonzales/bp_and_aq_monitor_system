//----------------------------------------------INCLUDES
#include <WiFi.h> // wifi library for esp32
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <limits.h>
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
// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(ONE_WIRE_BUS);
// Pass our oneWire reference to Dallas Temperature.
DallasTemperature sensors(&oneWire);
//----------------------------------------------FIREBASE OBJECTS
FirebaseData fbdo;
FirebaseData ahcStream;
FirebaseAuth auth;
FirebaseConfig config;

// Variable to save USER UID
String uid;
// Database main path
String dbPath;
//===============================[WATER MONITOR COMPONENT]
// Database parent paths
String dbHotTempPath;
String dbWaterMonitorPath;
String dbHotPipeBurstWarningPath;
String dbAtHomeCommandPath;
String dbIsAtHomePath;
String dbAtHomeCommandEventListenerPath; // path stream for automate command listener

// Database child nodes
String hTempPath = "/Data-Hot-Temp";
String hpbwPath = "/Warning-Hot-Pipe-Burst";
String ahcPath = "/Command-At-Home";
String iahPath = "/Is-At-Home";

// needed for millis function read/write at specified interval in a non blocking way
unsigned long tempSendDataPrevMillis = 0;
unsigned long tempReadMillis = 0;
// for esp32 timing
unsigned long espMillis = 0;
// boolean for signed in
bool signupOK = false;
// hardcoded device addresses for sensors
uint8_t roomTemperature[8] = {0x28, 0x8D, 0xFB, 0xCA, 0x5E, 0x14, 0x01, 0x4C};
uint8_t pipeTemperature[8] = {0x28, 0x95, 0x1C, 0xCD, 0x5E, 0x14, 0x01, 0xBB};

// Other variables
bool isAtHome = true;
bool atHomeCommand = true;
bool pipeBurstWarning = false;
bool lastPipeBurstWarning = pipeBurstWarning;
bool tempSensorsConnected = true;

const float TOL = 7.5; // how much hotter the pipe temp needs to be
float hotTemp = 0;
float roomTemp = 0;
int count = 0;
bool systemReset = true;
unsigned long warnLockMillis = 0;
unsigned long atHomeWarnLockMillis = 0;
bool warnLocked = true;
bool pipeBurstChanged = false;
bool lastPipeBurstChanged = pipeBurstChanged;
volatile bool ahcDataChanged = false;

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
  dbAtHomeCommandEventListenerPath = dbWaterMonitorPath + ahcPath;

  if (!Firebase.RTDB.beginStream(&ahcStream, dbAtHomeCommandEventListenerPath.c_str()))
    Serial.printf("Command-At-Home stream begin error, %s\n\n", ahcStream.errorReason().c_str());

  Firebase.RTDB.setStreamCallback(&ahcStream, ahcStreamCallback, ahcStreamTimeoutCallback);


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
    hotTemp = sensors.getTempC(pipeTemperature);
    roomTemp = sensors.getTempC(roomTemperature);

    // check if devices are connected then output temp data on the serial monitor
    if ((hotTemp != DEVICE_DISCONNECTED_C) && (roomTemp != DEVICE_DISCONNECTED_C))
    {
      tempSensorsConnected = true;
      Serial.printf("Hot Pipe Temperature is: %f\n", hotTemp);
      Serial.printf("Room Temperature is: %f", roomTemp);
    }
    else
    {
      tempSensorsConnected = false;
      Serial.println("Error: Devices are disconnected");
    }
  }

  // test the temp change for setting the boolean value in database
  // set pipe burst warning threshold
  if (abs(hotTemp - roomTemp) > TOL)
  {
    pipeBurstWarning = true;
    pipeBurstChanged = true;
  }
  else
  {
    pipeBurstWarning = false;
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
      dbHotPipeBurstWarningPath = dbWaterMonitorPath + hpbwPath;
      dbAtHomeCommandPath = dbWaterMonitorPath + ahcPath;
      dbIsAtHomePath = dbWaterMonitorPath + iahPath;
      dbHotTempPath = dbWaterMonitorPath + hTempPath;
      // Get current values from database
      Serial.printf("Checking iah node...%s\n", Firebase.RTDB.getBool(&fbdo, dbIsAtHomePath.c_str(), &isAtHome) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Checking ahc node...%s\n", Firebase.RTDB.getBool(&fbdo, dbAtHomeCommandPath.c_str(), &atHomeCommand) ? "ok" : fbdo.errorReason().c_str());
      // Set data nodes with values read from db
      Serial.printf("Initializing pbw node...%s\n", Firebase.RTDB.setBool(&fbdo, dbHotPipeBurstWarningPath.c_str(), pipeBurstWarning) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Initializing iah node...%s\n", Firebase.RTDB.setBool(&fbdo, dbIsAtHomePath.c_str(), isAtHome) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Initializing ahc node...%s\n", Firebase.RTDB.setBool(&fbdo, dbAtHomeCommandPath.c_str(), atHomeCommand) ? "ok" : fbdo.errorReason().c_str());
      Serial.printf("Initializing hot pipe temp node... %s\n", Firebase.RTDB.setFloat(&fbdo, dbHotTempPath.c_str(), hotTemp) ? "ok" : fbdo.errorReason().c_str());
      // finished reboot initializes
      systemReset = false;
    }
    // send temp data every 1 secs to the database
    if (millis() - tempSendDataPrevMillis > 1 * 1000 && tempSensorsConnected)
    {
      tempSendDataPrevMillis = millis(); // update prev timer with current time
      // update path for water pipes readings
      Serial.printf("Updating temp-hot-water-line... %s\n", Firebase.RTDB.setFloat(&fbdo, dbHotTempPath.c_str(), hotTemp) ? "ok" : fbdo.errorReason().c_str());
    }
    //___________________________________PIPE BURST WARNINGS
    //------------------------------------------AWAY FROM HOME Warning delays
    // Check if not at home is true, then send warning after 5 seconds stable value
    if (pipeBurstWarning != lastPipeBurstWarning && !isAtHome)
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
        lastPipeBurstWarning = pipeBurstWarning;
        // Update pipe burst warning path
        dbHotPipeBurstWarningPath = dbWaterMonitorPath + hpbwPath;
        Serial.printf("State change on warning, setting node to new state...%s\n", Firebase.RTDB.setBool(&fbdo, dbHotPipeBurstWarningPath.c_str(), pipeBurstWarning) ? "ok" : fbdo.errorReason().c_str());
        warnLocked = true;
      }
    }
    //------------------------------------------INSIDE HOME Warning delays
    // Check if at home is true and pipeBurstWarning changed to true, send after delayed amount of time
    if ((pipeBurstWarning != lastPipeBurstWarning) && isAtHome && pipeBurstWarning)
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
        lastPipeBurstWarning = pipeBurstWarning;
        // Update pipe burst warning path
        dbHotPipeBurstWarningPath = dbWaterMonitorPath + hpbwPath;
        Serial.printf("State change on warning, setting node to new state...%s\n", Firebase.RTDB.setBool(&fbdo, dbHotPipeBurstWarningPath.c_str(), pipeBurstWarning) ? "ok" : fbdo.errorReason().c_str());
        warnLocked = true;
      }
    }
    // Check if at home is true and pipeBurstWarning changed to false, send after 5 seconds stable value
    else if ((pipeBurstWarning != lastPipeBurstWarning) && isAtHome && !pipeBurstWarning)
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
        lastPipeBurstWarning = pipeBurstWarning;
        // Update pipe burst warning path
        dbHotPipeBurstWarningPath = dbWaterMonitorPath + hpbwPath;
        Serial.printf("State change on warning, setting node to new state...%s\n", Firebase.RTDB.setBool(&fbdo, dbHotPipeBurstWarningPath.c_str(), pipeBurstWarning) ? "ok" : fbdo.errorReason().c_str());
        warnLocked = true;
      }
    }
    //___________________________________EVENT LISTENER AND ACTION
    //listen for at home commands
    if (ahcDataChanged)
    { // change booleans when at home command node changes
      ahcDataChanged = false;
      //
      if (ahcStream.dataTypeEnum() == fb_esp_rtdb_data_type_boolean)
      {
        Serial.print("Received at home command state: ");
        Serial.println(ahcStream.to<bool>() == 1 ? "HOME" : "AWAY");
        if (ahcStream.to<bool>() == true)
        {
          isAtHome = true;
          // Successfully changed  state, update database node /Is-At-Home
          Serial.printf("Updating iah node...%s\n", Firebase.RTDB.setBool(&fbdo, dbIsAtHomePath.c_str(), isAtHome) ? "ok" : fbdo.errorReason().c_str());
        }
        else if (ahcStream.to<bool>() == false)
        {
          isAtHome = false;
          // Successfully changed valve state, update database node /Is-At-Home
          Serial.printf("Updating iah node...%s\n", Firebase.RTDB.setBool(&fbdo, dbIsAtHomePath.c_str(), isAtHome) ? "ok" : fbdo.errorReason().c_str());
        }
      }
    }
  }

  // increment count every second
  if (millis() - espMillis > 300 * 1000 )
  {
    espMillis = millis();
    systemReset = true;
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

//------------------------------------------------------------------------------iahStreamCallback()
// Global function that handles at home command stream data
void ahcStreamCallback(FirebaseStream data)
{
  Serial.printf("Stream path, %s\nevent path, %s\ndata type, %s\nevent type, %s\n\n",
                data.streamPath().c_str(),
                data.dataPath().c_str(),
                data.dataType().c_str(),
                data.eventType().c_str());
  printResult(data);
  Serial.println();
  Serial.printf("Received stream payload size: %d (Max. %d)\n\n", data.payloadLength(), data.maxPayloadLength());
  ahcDataChanged = true;
} // ahcStreamCallBack()
//------------------------------------------------------------------------------ascStreamTimeoutCallback()
void ahcStreamTimeoutCallback(bool timeout)
{
  if (timeout)
    Serial.println("stream timed out, resuming...\n");
  if (!ahcStream.httpConnected())
    Serial.printf("error code: %d, reason: %s\n\n", ahcStream.httpCode(), ahcStream.errorReason().c_str());
} // ahcStreamTimeoutCallback
