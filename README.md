# ESP8266 MQTT Weather
* DSTH01 humidity sensor
* BMP180 pressure & temperature sensor
* ESP 8266 (e.g. [Adafruit HUZZAH](https://www.adafruit.com/products/2471))
* Sensors communicate over I2C
* ESP 8266 connects to local WiFi, and then to [Adafruit IO](https://io.adafruit.com)
via [MQTT](http://www.hivemq.com/blog/mqtt-essentials-part-1-introducing-mqtt)

Module wakes up every minute, collects measurements for 3 seconds,
and goes back to low-power deep sleep.
