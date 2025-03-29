#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TimeLib.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "thingProperties.h"
#include <EEPROM.h>
#include <Stepper.h>
#define EEPROM_SIZE 2 

const char* ssid = ""; // WiFi name
const char* password = ""; // WiFi password
const char* googleScriptUrl = "";

const char* api_key = "";

const int stepsPerRevolution = 200;

Stepper myStepper1(stepsPerRevolution, 4, 6, 5, 7);
Stepper myStepper2(stepsPerRevolution, 15, 17, 16, 18);
Stepper myStepper3(stepsPerRevolution, 11, 13, 12, 14);

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -7 * 3600, 60000); // Update every 60 seconds

String deviceID = "";
unsigned long previousOpenAITime = 0;  // Tracks last OpenAI call time
const unsigned long openAIDelay = 5 * 60 * 1000;  // every x minutes

// Timing for logging battery
unsigned long previousLogTime = 0;
const unsigned long logInterval = 10000; // log every 10 seconds

// Analog pin for the sound sensor
const int soundSensorPin = A0;

void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);
  //pinMode(LED_PIN, OUTPUT);

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
  }
  Serial.println("\nConnected to WiFi!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  myStepper1.setSpeed(20);
  myStepper2.setSpeed(20);
  myStepper3.setSpeed(20);

  pinMode(1, OUTPUT);//for buzzer

  // Initialize NTP client
  timeClient.begin();
  timeClient.update(); // Force an initial time update

  initProperties();
  ArduinoCloud.begin(ArduinoIoTPreferredConnection);
  setDebugMessageLevel(2);
  ArduinoCloud.printDebugInfo();
}

// Function to get formatted Date-Time String (YYYY-MM-DD HH:MM:SS)
String getFormattedDateTime() {
    timeClient.update(); // Ensure the time is up to date
    time_t rawTime = timeClient.getEpochTime();  // Get Unix time
    struct tm *timeinfo = localtime(&rawTime);   // Convert to readable format
    
    char buffer[20];  
    sprintf(buffer, "%04d-%02d-%02d %02d:%02d:%02d", 
            timeinfo->tm_year + 1900,  // Convert from years since 1900
            timeinfo->tm_mon + 1,       // Month is 0-indexed, so add 1
            timeinfo->tm_mday, 
            timeinfo->tm_hour, 
            timeinfo->tm_min, 
            timeinfo->tm_sec);
    
    return String(buffer);
}

void checkWiFi() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi lost! Attempting to reconnect...");

        WiFi.disconnect();
        WiFi.begin(ssid, password);

        while (WiFi.status() != WL_CONNECTED) {  // Keep trying until connected
            delay(1000);
            Serial.print(".");
        }

        Serial.println("\nWiFi Reconnected!");
        Serial.print("New IP address: ");
        Serial.println(WiFi.localIP());
    }
}

// Function to log battery data to Google Sheets
void logbatteryToGoogleSheets(String timestamp, float soundValue) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(googleScriptUrl);
    http.addHeader("Content-Type", "application/json");
    
    // Create JSON payload
    String jsonData = "{\"deviceId\":\"" + String(deviceID) +
                      "\", \"timestamp\":\"" + timestamp +
                      "\", \"battery\":" + String(soundValue) + "}";
    
    int httpResponseCode = http.POST(jsonData);
    Serial.print("Google Sheets Response code: ");
    Serial.println(httpResponseCode);
    http.end();
  } else {
    Serial.println("WiFi not connected.");
  }
}

void callOpenAI() {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;

        // 🔹 Fetch timestamps and locations from Google Sheets for the last 3 or 15 hours
        String fetchUrl = String(googleScriptUrl) + "?deviceId=" + deviceID;

        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // 🔹 Enable redirect handling
        http.begin(fetchUrl);
        int fetchResponseCode = http.GET();

        String timestampsString = "[]";
        String batteryString = "[]";

        if (fetchResponseCode > 0) {
            String fetchResponse = http.getString();
            Serial.print("Google Sheets Response: ");
            Serial.println(fetchResponse);

            DynamicJsonDocument fetchJson(4096);  // Increased size to fit timestamps & locations
            deserializeJson(fetchJson, fetchResponse);

            JsonArray timestamps = fetchJson["timestamps"];
            JsonArray batteries = fetchJson["battery"];

            // Format timestamps
            timestampsString = "[";
            for (size_t i = 0; i < timestamps.size(); i++) {
                timestampsString += "\"" + String(timestamps[i].as<const char*>()) + "\"";
                if (i < timestamps.size() - 1) {
                    timestampsString += ", ";
                }
            }
            timestampsString += "]";

            // --- Format battery ---
            batteryString = "[";
            for (size_t i = 0; i < batteries.size(); i++) {
              batteryString += "\"" + String(batteries[i].as<float>()) + "\"";
              if (i < batteries.size() - 1) {
                batteryString += ", ";
              }
            }
            batteryString += "]";
        } else {
            Serial.print("Failed to fetch data, HTTP error: ");
            Serial.println(fetchResponseCode);
        }
        http.end();

        timeClient.update();
        int currentHour = timeClient.getHours();
        int hoursToAnalyze = (currentHour == 12) ? 15 : 3;

        // 🔹 Send timestamps, locations, and home location to OpenAI for decision making
        http.begin("https://api.openai.com/v1/chat/completions");
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Authorization", String("Bearer ") + api_key);

        // Create JSON request
        DynamicJsonDocument jsonDoc(4096);
        jsonDoc["model"] = "gpt-4o-mini";
        JsonArray messages = jsonDoc.createNestedArray("messages");

        JsonObject systemMessage = messages.createNestedObject();
        systemMessage["role"] = "system";
        systemMessage["content"] =
          String("Analyze the user's battery data from the past ") + String(hoursToAnalyze) + " hours. "
          "If the battery is less than or equal to 20%, return 0. "
          "If the battery is greater than or equal to 80%, return 5760. "
          "If the battery is between 20% and 80%:\n"
          "    - If there is a growing trend (charging), return a positive value in (0, 1920). "
          "    - If there is a decaying trend (consuming), return a positive value in [3840, 5760). "
          "    - Otherwise, if there is no significant trend, return a positive value in [1920, 3840). "
          "Your response must be exactly in this format:\n\n"
          "<integer> - <short reason under 200 characters>\n\n"
          "For example:\n"
          "0 - Battery is lower than 20%.\n"
          "5760 - Battery is greater than or equal to 80%.\n"
          "1000 - Battery is between 20% and 80% and the user is charging the phone.\n"
          "5000 - Battery is between 20% and 80% and the battery is being consumed.\n"
          "3000 - Battery is between 20% and 80% and there is no significant trend in the battery data.\n";

        JsonObject userMessage = messages.createNestedObject();
        userMessage["role"] = "user";
        userMessage["content"] =
          String("We have these timestamps: ") + timestampsString +
          ", with battery levels: " + batteryString +
          ". Return a single integer first, followed by a short reason (<200 chars), in the format '<integer> - <short reason>'.";

        String requestBody;
        serializeJson(jsonDoc, requestBody);

        int httpResponseCode = http.POST(requestBody);

        if (httpResponseCode > 0) {
            String response = http.getString();
            //Serial.print("OpenAI Response: ");
            //Serial.println(response);

            int finalScore = 0;
            String reason = "";

            DynamicJsonDocument responseJson(1024);
            deserializeJson(responseJson, response);
            if (responseJson["choices"][0]["message"]["content"]) {
                String gptReply = responseJson["choices"][0]["message"]["content"];
                Serial.print("Raw GPT Reply: ");
                Serial.println(gptReply);

                // Extract first integer from response
                int separatorIndex = gptReply.indexOf(" - ");
                if (separatorIndex > 0) {
                    finalScore = gptReply.substring(0, separatorIndex).toInt();
                    reason = gptReply.substring(separatorIndex + 3); // Extract text after " - "
                }
            }

            Serial.print("Calculated Battery Score: ");
            Serial.println(finalScore);
            Serial.print("Reason: ");
            Serial.println(reason);

          int gptValue = finalScore;

          // Actions after getting value from GPT
          // 1) Beep for 2 seconds
          beepBuzzer(2000);  // 2000 ms = 2 seconds

          // 2) Reset rotation to the starting position by rotating 5760 steps
          rotateSteppers(5760);

          // 3) Rotate by the negative GPT value
          rotateSteppers(-gptValue);

          // 4) Store the GPT value in EEPROM
          EEPROM.put(0, gptValue);
          EEPROM.commit();

            // 🔹 Commented out LED control for now
            /*
            if (travelScore > 0) {
                Serial.println("Blinking the LED...");
                for (int i = 0; i < travelScore / 100; i++) { // Adjust intensity if needed
                    digitalWrite(LED_PIN, HIGH);
                    delay(1000);
                    digitalWrite(LED_PIN, LOW);
                    delay(1000);
                }
            } else {
                Serial.println("Keeping the LED ON.");
                digitalWrite(LED_PIN, HIGH);
            }
            */
        } else {
            Serial.print("HTTP Error code: ");
            Serial.println(httpResponseCode);
        }

        http.end();
    } else {
        Serial.println("WiFi not connected.");
    }
}

unsigned long lastBatteryLogTime = 0;
const unsigned long batteryLogInterval = 30 * 60 * 1000; // 30 minutes

void loop() {
  checkWiFi();
  ArduinoCloud.update();
  timeClient.update();

  int currentHour = timeClient.getHours();
  int currentMinute = timeClient.getMinutes();
  unsigned long currentMillis = millis();

  // 🔹 Call OpenAI at specific times
  if ((currentHour == 12 || currentHour == 15 || currentHour == 18 || currentHour == 21) && currentMinute == 0) {
    Serial.println("Triggering OpenAI decision based on scheduled time...");
    callOpenAI();
    delay(60000); // Avoid multiple calls in the same minute
  }

  // 🔹 Log battery every 30 minutes (regardless of time)
  if (currentMillis - lastBatteryLogTime >= batteryLogInterval) {
    lastBatteryLogTime = currentMillis;

    String timestamp = getFormattedDateTime();
    Serial.print("Logging battery at ");
    Serial.print(timestamp);
    Serial.print(": ");
    Serial.println(battery);

    logbatteryToGoogleSheets(timestamp, battery);
  }

  delay(100); // small delay to ease CPU load
}

// Function to make the buzzer beep once
void beepBuzzer(int duration) {
    digitalWrite(1, HIGH);
    delay(duration);
    digitalWrite(1, LOW);
}

/**
* Rotates both stepper motors by the given angle.
* @param angle The rotation angle in degrees (positive for CW, negative for CCW).
*/
void rotateSteppers(int angle) {
  int steps = (angle * stepsPerRevolution) / 360;
  
  Serial.print("Rotating by: ");
  Serial.print(angle);
  Serial.println(" degrees");
  
  for (int i = 0; i < abs(steps); i++) {
    if (steps > 0) {
      myStepper1.step(1);
      myStepper2.step(1);
      myStepper3.step(1);
    } else {
      myStepper1.step(-1);
      myStepper2.step(-1);
      myStepper3.step(-1);
    }
    delay(2);
  }
  
  Serial.println("Rotation Complete.");
}
// Define the callback (if needed) when the cloud value changes.
void onBatteryChange() {
  // Optional: add code to handle changes if necessary.
}


