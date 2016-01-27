/***************************************************
  Adafruit MQTT Library ESP8266 Example

  Must use ESP8266 Arduino from:
    https://github.com/esp8266/Arduino

  Works great with Adafruit's Huzzah ESP board & Feather
  ----> https://www.adafruit.com/product/2471
  ----> https://www.adafruit.com/products/2821

  Adafruit invests time and resources providing this open source code,
  please support Adafruit and open-source hardware by purchasing
  products from Adafruit!

  Written by Tony DiCola for Adafruit Industries.
  MIT license, all text above must be included in any redistribution
 ****************************************************/
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <Adafruit_MQTT.h>
#include <Adafruit_MQTT_Client.h>

#include <SimpleTimer.h>
#include <SFE_BMP180.h>
#include <DSTH01.h>

#include <Wire.h>
#include <EEPROM.h>

// cumulative statistics library (i.e. not ring buffer)
// from https://github.com/igg2/CumulStats
#include <CumulStats.h>


#undef ADC_CAL
//#define ADC_CAL
#undef DEBUG
#define DEBUG
#include "printDefn.h"
/************************* WiFi Access Point *********************************/

#define WLAN_SSID       "Radio Free Cathilya"
#define WLAN_PASS       ""
#define HOSTNAME       "espWeather01"
char localMAC[18];

/************************* OTA updates *********************************/
const bool updateHttps = false;
#define updateServer "cathilya2.home.cathilya.org"

/************************* Adafruit.io Setup *********************************/

#define AIO_SERVER      "io.adafruit.com"
#define AIO_SERVERPORT  1883                   // use 8883 for SSL
#define AIO_USERNAME    "iggie"
#define AIO_KEY         "13acf38e7da23a44509f362d4efa18c79429a483"

/************************* Pin Setup *********************************/
const int ledPin1   = 0;      // the number of the Red LED pin
const int ledPin2   = 2;      // the number of the Blue LED pin
const int I2c3v3Pin = 13;     // pin with PFET controlling I2C V+
const int SDA_PIN   = 4;
const int SCL_PIN   = 5;

// setup the EEPROM and default conf
const byte eeprom_magic = 0x43;
struct eepromConf {
  byte magic;
  double VBAT_CAL_M;
  double VBAT_CAL_B;
  bool   heatedRH;
  char OTA_MD5[33];
  char bootReason[127];
};

// resistor divider to divide-by-6:
// parallel 1M 5% to V+ (=500k), 100k 1% to GND
// Calibration observations:
// ADCcounts Vbat
//   0.0   0.0
// 594.745 3.505
// 595.908 3.516
// 586.767 3.553 
// 585.633 3.549 
// 579.933 3.553 
// 582.067 3.554 
// 583.467 3.557
// 861.550 5.18
// 866.900 5.19
// y = 0.0059961786x + 0.0157485175
const struct eepromConf eepromDefConf = {
  eeprom_magic,
  0.0059961786,
  0.0157485175,
  false,
  "Non-OTA",
  "Flash"
};
struct eepromConf eepromConf;
const int zeroMD5[16] = {0};

// the timer object
SimpleTimer mainTimer;
// ms between timer calls
const int tick_ms = 50;
// start at one to avoid doing too much in the first call
int tickCount = 1;
#define DEEP_SLEEP_SECS 60


/************ I2C Sensor setup ******************/
SFE_BMP180 I2C_BMP180;
DSTH01     I2C_DSTH01;

int I2Cstate = 0;
#define I2C_NSTATES    3
#define I2C_1  0
#define I2C_2  1
#define I2C_3  2

// Number of complete I2C cycles to collect before publishing
#define I2C_Sample_Seconds 3
const int I2C_Publish_Cycles = (I2C_Sample_Seconds * 1000) / (tick_ms * I2C_NSTATES);
int I2Ccycle = 0;

// for BMP sensor
CumulStats temperature;
CumulStats pressure;
// Altitude in meters @ home
#define ALTITUDE 17.0
// for DST sensor
CumulStats humidity;
// Vbat from ADC A0
CumulStats Vbat;
CumulStats ADCcounts;


/************ Global State (you don't need to change this!) ******************/

// Create an ESP8266 WiFiClient class to connect to the MQTT server.
WiFiClient client;
// or... use WiFiFlientSecure for SSL
//WiFiClientSecure client;

// Store the MQTT server, username, and password in flash memory.
// This is required for using the Adafruit MQTT library.
const char MQTT_SERVER[] PROGMEM    = AIO_SERVER;
const char MQTT_USERNAME[] PROGMEM  = AIO_USERNAME;
const char MQTT_PASSWORD[] PROGMEM  = AIO_KEY;

// Setup the MQTT client class by passing in the WiFi client and MQTT server and login details.
Adafruit_MQTT_Client mqtt(&client, MQTT_SERVER, AIO_SERVERPORT, MQTT_USERNAME, MQTT_PASSWORD);

/****************************** MQTT Feeds ***************************************/

// Setup a feed called 'photocell' and button for publishing.
// Notice MQTT paths for AIO follow the form: <username>/feeds/<feedname>
const char TEMPERATURE_FEED[] PROGMEM = AIO_USERNAME "/feeds/temperature";
Adafruit_MQTT_Publish temperature_feed = Adafruit_MQTT_Publish(&mqtt, TEMPERATURE_FEED);
const char PRESSURE_FEED[] PROGMEM = AIO_USERNAME "/feeds/pressure";
Adafruit_MQTT_Publish pressure_feed = Adafruit_MQTT_Publish(&mqtt, PRESSURE_FEED);
const char HUMIDITY_FEED[] PROGMEM = AIO_USERNAME "/feeds/humidity";
Adafruit_MQTT_Publish humidity_feed = Adafruit_MQTT_Publish(&mqtt, HUMIDITY_FEED);
const char VBAT_FEED[] PROGMEM = AIO_USERNAME "/feeds/Vbat";
Adafruit_MQTT_Publish Vbat_feed = Adafruit_MQTT_Publish(&mqtt, VBAT_FEED);
const char LOG_FEED[] PROGMEM = AIO_USERNAME "/feeds/log";
Adafruit_MQTT_Publish log_feed = Adafruit_MQTT_Publish(&mqtt, LOG_FEED);

// Setup subscriptions.
const char LED1_FEED[] PROGMEM = AIO_USERNAME "/feeds/led1";
Adafruit_MQTT_Subscribe led1 = Adafruit_MQTT_Subscribe(&mqtt, LED1_FEED);
const char HEATED_RH[] PROGMEM = AIO_USERNAME "/feeds/heated_rh";
Adafruit_MQTT_Subscribe heatedRH = Adafruit_MQTT_Subscribe(&mqtt, HEATED_RH);

const int snprintfBufLen = 128;
char snprintfBuf[snprintfBufLen];
/*************************** Function Declarations ************************************/
void checkUpdate();
void connectWiFi();
void do_ADC();
void doMQTT_Subscriptions ();
void do_I2C ();
void doMQTT_Publish();
void doLEDflash();
void MQTT_connect ();
void updateEEPROM ();
void nap();

/*************************** Sketch Code ************************************/

// Bug workaround for Arduino 1.6.6, it seems to need a function declaration
// for some reason (only affects ESP8266, likely an arduino-builder bug).

void timedRepeat () {
	tickCount++;
	do_ADC();
	do_I2C();
	doMQTT_Publish();
	doMQTT_Subscriptions();
	doLEDflash();
}


void setup() {
#if defined (DEBUG) || defined (ADC_CAL)
	Serial.begin(115200);
#endif

	delay(10);

	EEPROM.begin(4096);
	EEPROM.get (0, eepromConf);
  if (eepromConf.magic != eeprom_magic) {
  	Debugprintf("Writing default conf...");
  	EEPROM.put (0, eepromDefConf);
  	EEPROM.commit();
  	EEPROM.get (0, eepromConf);
  	Debugprintf("New magic: %d\n", eepromConf.magic);
  }

	Debugprintf("Weatherstation demo\n\n");

	// Set up DIO
  // Turn on I2C power
  pinMode(I2c3v3Pin, OUTPUT);
  digitalWrite(I2c3v3Pin,  LOW);
	// led1 on until wifi connects
  pinMode(ledPin1, OUTPUT);
	digitalWrite(ledPin1,  LOW);
	// led2 off
	pinMode(ledPin2, OUTPUT);
	digitalWrite(ledPin2,  HIGH);

	// Set up ADC pin
	pinMode(A0, INPUT);

	// Connect to WiFi access point.
	// This will either connect after several attempts or call nap()
	connectWiFi();

	// Signal connection by turning off LED
	digitalWrite(ledPin1,  HIGH);

	String mac = WiFi.macAddress();
	mac.replace(':', '-');
	snprintf (localMAC, sizeof(localMAC), "%s", mac.c_str());
	Debugprintf("\nWiFi connected, IP: %d.%d.%d.%d, MAC: %s\n", WiFi.localIP()[0], WiFi.localIP()[1], WiFi.localIP()[2], WiFi.localIP()[3], localMAC);
	
	// Check if there's an OTA update
	// checkUpdate();

	// Establish a connection to the broker
	// We to do this in setup to log early sensor problems
	MQTT_connect();

	// Setup MQTT subscriptions.
  mqtt.subscribe(&led1);
  mqtt.subscribe(&heatedRH);

	// timed loop
	mainTimer.setInterval(tick_ms, timedRepeat);


	// startup the sensors
	// The first call to Wire.begin(sda,scl) sets up the default pins
	// Which will be used if Wire.begin() gets called again without params
	Wire.begin (SDA_PIN, SCL_PIN);

	// BMP180 Temperature and Pressure
	if (I2C_BMP180.begin()) {
		Debugprintf("\nBMP180 init success\n");
	} else {
		// Oops, something went wrong, this is usually a connection problem,
		// see the comments at the top of this sketch for the proper connections.
		Debugprintf("BMP180 initial startup failed (error %d): REBOOT\n", I2C_BMP180.getError());
		logFeedPrintf("BMP180 initial startup failed (error %d): REBOOT", I2C_BMP180.getError());
		nap();
	}
	Debugprintf("provided altitude: %s meters, ", dtostrf ( ALTITUDE, 0, 1, snprintfBuf));
	Debugprintf("%s feet\n", dtostrf ( ALTITUDE*3.28084, 0, 1, snprintfBuf));
 
	// DSTH01 Humidity
	// We read the onboard temperature to compensate for the humidity with the heater on.
	// We ignore this temperature reading for the report though.
	if (I2C_DSTH01.detectSensor()) {
		// The sensor is detected. Lets carry on!
		Debugprintf("\nDSTH01 detected\n"); 
	} else {
		Debugprintf("DSTH01 initial startup failed: REBOOT\n");
		logFeedPrintf("DSTH01 initial startup failed: REBOOT");
		nap();
	}
  if (eepromConf.heatedRH) {
    I2C_DSTH01.enableHeater();
  } else {
    I2C_DSTH01.disableHeater();
  }


	Debugprintf("Starting loop...\n\n");
	Serial.flush();
}


void loop() {
	// Ensure the connection to the MQTT server is alive (this will make the first
	// connection and automatically reconnect when disconnected).  See the MQTT_connect
	// function definition further below.
	MQTT_connect();
	mainTimer.run();

	// ping the server to keep the mqtt connection alive
	// NOT required if you are publishing once every KEEPALIVE seconds (=300)
	/*
	if(! mqtt.ping()) {
		mqtt.disconnect();
	}
	*/
}

void checkUpdate () {

	snprintf (snprintfBuf, snprintfBufLen, "/ESP8266OTA/%s/%s/firmware.bin", localMAC, eepromConf.OTA_MD5);
	Debugprintf("Checking for update at http%s://%s%s\n", (updateHttps ? "s":""), updateServer, snprintfBuf);
	Serial.flush();
	int ret = ESPhttpUpdate.update(updateServer, 80, snprintfBuf);

	switch(ret) {
		case HTTP_UPDATE_FAILED:
			Debugprintf("[update] Update failed.\n");
			Serial.flush();
			break;
		case HTTP_UPDATE_NO_UPDATES:
			Debugprintf("[update] Update no Update.\n");
			Serial.flush();
			break;
		case HTTP_UPDATE_OK:
			Debugprintf("[update] Update ok.\n");
			Debugprintf("Updating MD5 from %s ... ", eepromConf.OTA_MD5);
			Serial.flush();

			strncpy (eepromConf.OTA_MD5, Update.md5String().c_str(), sizeof(eepromConf.OTA_MD5));
			updateEEPROM();
			Debugprintf("eeprom MD5 updated to %s\n", eepromConf.OTA_MD5);
			Serial.flush();
			delay (100);
			logFeedPrintf("eeprom MD5 updated to %s", eepromConf.OTA_MD5);
			delay (100);
			ESP.restart();
			break;
	}
	delay (100);
}

void do_ADC () {
	double adc_read;

	adc_read = analogRead(A0);
	ADCcounts.add ( adc_read );
	adc_read = ( adc_read * eepromConf.VBAT_CAL_M ) + eepromConf.VBAT_CAL_B;
	Vbat.add ( adc_read );
}

const int ledFlashTicks = 1000/tick_ms;
int ledFlashState = 0;
void doLEDflash() {
  if (ledFlashState % ledFlashTicks == 0) {
      digitalWrite(ledPin2,  LOW);
  } else {
      digitalWrite(ledPin2,  HIGH);
  }
  ledFlashState = (ledFlashState+1) % ledFlashTicks;
}


void do_I2C () {
  double read_temp, read_pres, read_rh;
  int I2Cerr;

  if ( (I2Cerr = I2C_BMP180.getError()) ) {
    if (I2C_BMP180.begin()) {
      snprintf (snprintfBuf, snprintfBufLen, "BMP180 init success after I2C error %d", I2Cerr);
      Debugprintf("%s\n",snprintfBuf);
      log_feed.publish ( snprintfBuf );
    } else {
      snprintf (snprintfBuf, snprintfBufLen, "BMP180 init failed after I2C error %d: REBOOT", I2Cerr);
      Debugprintf("%s\n",snprintfBuf);
      log_feed.publish ( snprintfBuf );
      nap();
    }
  }
  switch (I2Cstate) {
    case I2C_1:
      I2C_BMP180.startTemperature();
      I2C_DSTH01.startTemperature();
      I2Cstate = I2C_2;
    break;

    case I2C_2:
      I2C_BMP180.getTemperature (read_temp);
      temperature.add (read_temp);
      I2C_DSTH01.readTemperature ();
      // the constant 3 is the longest/most precise pressure reading, taking ~25ms.
      I2C_BMP180.startPressure (3);
      I2C_DSTH01.startHumidity ();
      I2Cstate = I2C_3;
    break;

    case I2C_3:
      read_rh = I2C_DSTH01.readHumidity ();
      humidity.add ( read_rh );

      read_temp = temperature.mean();
      I2C_BMP180.getPressure (read_pres, read_temp);
      read_pres = I2C_BMP180.sealevel (read_pres, ALTITUDE);
      pressure.add ( read_pres );

      I2Cstate = I2C_1;
      I2Ccycle++;
    break;

    default:
      nap (); // Some bad juju happen here
    break;
  }
}


void doMQTT_Publish() {
  if ( I2Ccycle && (I2Ccycle % I2C_Publish_Cycles == 0) ) {
    // Now we can publish stuff!
    dtostrf  ( temperature.mean(), 0, 2, snprintfBuf);
    Debugprintf("Sending temperature %s...", snprintfBuf);
    if (! temperature_feed.publish( snprintfBuf ) ) {
      Debugprintf("Failed\n");
    } else {
      Debugprintf("OK!\n");
    }

    dtostrf  ( pressure.mean(), 0, 2, snprintfBuf);
    Debugprintf("Sending pressure %s...", snprintfBuf);
    if (! pressure_feed.publish(snprintfBuf) ) {
      Debugprintf("Failed\n");
    } else {
      Debugprintf("OK!\n");
    }

    dtostrf  ( Vbat.mean(), 0, 3, snprintfBuf);
    Debugprintf("Sending Vbat %s...", snprintfBuf);
    if (! Vbat_feed.publish(snprintfBuf) ) {
      Debugprintf("Failed\n");
    } else {
      Debugprintf("OK!\n");
    }

    ADCprintf("\nVbat\tADC counts\n%.3f\t%.3f", Vbat.mean(), ADCcounts.mean());

    dtostrf  ( humidity.mean(), 0, 1, snprintfBuf);
    Debugprintf("Sending RH %s...", snprintfBuf);
    if (! humidity_feed.publish(snprintfBuf) ) {
      Debugprintf("Failed\n");
    } else {
      Debugprintf("OK!\n");
    }

  // All that publishing! Time for a nap.
  nap();
  }
}

void doMQTT_Subscriptions () {
  int ledPin=-1;
  // this is our 'wait for incoming subscription packets' busy subloop
  // try to spend your time here

  Adafruit_MQTT_Subscribe *subscription;
  while ((subscription = mqtt.readSubscription())) {
    if (subscription == &led1) {
      ledPin = ledPin1;
    } else if (subscription == &heatedRH) {
      if (! strcmp ((char *)subscription->lastread, "ON") ) {
        eepromConf.heatedRH = true;
        I2C_DSTH01.enableHeater();
        updateEEPROM();
      } else {
        eepromConf.heatedRH = false;
        I2C_DSTH01.disableHeater();
        updateEEPROM();
      }
    }

    if (ledPin >= 0 ) {
      Debugprintf("LED %d: %s\n", ledPin, (char *)subscription->lastread);
      if (! strcmp ((char *)subscription->lastread, "ON") ) {
        digitalWrite(ledPin, LOW);
      } else if (! strcmp ((char *)subscription->lastread, "OFF") ) {
        digitalWrite(ledPin, HIGH);
      }
    }
  }

}


void connectWiFi() {
  Debugprintf("Connecting to %s\n", WLAN_SSID);

  WiFi.mode (WIFI_STA);
  WiFi.hostname (HOSTNAME);
  WiFi.begin (WLAN_SSID, WLAN_PASS);
  int beginRetries = 5;
	while (beginRetries && WiFi.status() != WL_CONNECTED) {
		delay(100);
		int retries = 20;
		while (retries && WiFi.status() != WL_CONNECTED) {
			delay(100);
			retries--;
		}
		if (WiFi.status() != WL_CONNECTED) {
			Debugprintf("Retrying connection... (%d more)\n", beginRetries);
			WiFi.begin (WLAN_SSID, WLAN_PASS);
			beginRetries--;
		}
	}
	if (!beginRetries && WiFi.status() != WL_CONNECTED) {
		Debugprintf("\nGiving up on WiFi: REBOOT\n");
		nap();
	}

}



// Function to connect and reconnect as necessary to the MQTT server.
// Should be called in the loop function and it will take care if connecting.
void MQTT_connect() {
  int8_t ret;

  // Stop if already connected.
  if (mqtt.connected()) {
    return;
  }

  Debugprintf("Connecting to MQTT... ");

  uint8_t retries = 5;
  while ((ret = mqtt.connect()) != 0) { // connect will return 0 for connected
        
    Serial.println(mqtt.connectErrorString(ret));
    Debugprintf("\nRetrying MQTT connection in 5 seconds...\n");
    mqtt.disconnect();
    delay(5000);  // wait 5 seconds
    retries--;
    if (retries == 0) {
      // basically die and wait for WDT to reset me
      nap();
    }
  }
  Debugprintf("MQTT Connected!\n");
}

void updateEEPROM () {
//  struct eepromConf {
//    byte magic;
//    double VBAT_CAL_M;
//    double VBAT_CAL_B;
//    bool   heatedRH;
//    char OTA_MD5[33];
//    char bootReason[127];
//  };

  struct  eepromConf eepromConfTest;
  EEPROM.get (0, eepromConfTest);
  if ( (eepromConf.magic != eepromConfTest.magic) |
    (eepromConf.VBAT_CAL_M != eepromConfTest.VBAT_CAL_M) |
    (eepromConf.VBAT_CAL_B != eepromConfTest.VBAT_CAL_B) |
    (eepromConf.heatedRH != eepromConfTest.heatedRH) |
    (strncmp (eepromConf.OTA_MD5, eepromConfTest.OTA_MD5, sizeof(eepromConf.OTA_MD5)) ) |
    (strncmp (eepromConf.bootReason, eepromConfTest.bootReason, sizeof(eepromConf.bootReason)) )
    ) {
      EEPROM.put (0, eepromConf);
      EEPROM.commit();
      EEPROM.get (0, eepromConf);
      Debugprintf("Updated EEPROM");
    }
}

void nap() {
  // Set up DIO
  // Turn off I2C power & LEDs
  if (!eepromConf.heatedRH)
    digitalWrite(I2c3v3Pin,  HIGH);
  digitalWrite(ledPin1,  HIGH);
  digitalWrite(ledPin2,  HIGH);


  I2Ccycle = 0;
  temperature.reset();
  pressure.reset();
  humidity.reset();
  Vbat.reset();
  ADCcounts.reset();

  mqtt.disconnect();
  ESP.deepSleep( (1000 * 1000 * DEEP_SLEEP_SECS), WAKE_RF_DEFAULT);
}
