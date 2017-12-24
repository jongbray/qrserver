/*
 BITS KNOWN WORKING THIS BUILD:
  Server
  FSR
  RF
  R1
  LED1
  LED2
 */

#include <Dhcp.h>
#include <Dns.h>
#include <Ethernet.h>
#include <EthernetClient.h>
#include <EthernetServer.h>
#include <EthernetUDP.h>
#include <stdlib.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <WiFiClient.h>
#include "RCSwitch.h"

 //Server stuff
 
const char *ssid = "Dinky-TP";
const char *wifipassword = "!D10path0l0gy";
const char *ntphost = "utcnist2.colorado.edu";
const int SERVER_PORT = 82;
const int WOL_PORT = 7;

byte mac[] = {0x38, 0x2C, 0x4A, 0xBF, 0x00, 0xEF};
IPAddress wol_ip(192, 168, 1, 255); //Magic packet target - need to use broadcast address (ie normally x.x.x.25)
WiFiUDP udp;
MDNSResponder mdns;
ESP8266WebServer server(SERVER_PORT);

 
//Pin constants
 
	 //Anything from 2-5 (D0,D2,D4,D5) should be OK for digital outputs (analogRead knows you're dealing with analog pins anyway, and the only one on ESPDuino is 0)

const int PIN_RF_IN = 4;
const int PIN_RF_OUT = 5;
const int PIN_RELAY_1 = 12; //Pins 0,1,2 cause issues when rebooting, pin 3 didn't seem to work properly
const int PIN_IR_1 = 14;  //use 3.3V not 5V
const int PIN_IR_2 = 15;  //use 3.3V not 5V
const int PIN_IR_3 = 16;  //use 3.3V not 5V


//EEPROM Memory locations
const int eeprom_PT = 0;
const int eeprom_PW = 1;
const int eeprom_blanketDelay = 2;
const int eeprom_blanketCooltemp = 3;

//Relay stuff
int blanketDelay;                     //Blanket delay in minutes
int blanketCooltemp = 2;              //PW that blanket is set to after the delay
long blanketTime = 0;                 //The number of cycles remaining before blanket switched down
byte pulsewidth_r1;                   //Current pulsewidth of the blanket relay
String status_relay = "OFF";

//RF Stuff
String status_RF[7] = {"OFF", "OFF", "OFF", "OFF", "OFF", "OFF", "OFF"};
char *off_RF[7] = {"10110000000000000000", "10110000000010000010", "10110000000011000011", "10110000000001000001", "101100001110011000000100", "101100001110011000000010", "101100001110011000000001"};
char *on_RF[7] = {"10110000000000010001", "10110000000010010011", "10110000000011010010", "10110000000001010000", "101100001110011000001100", "101100001110011000001010", "101100001110011000001001"};
long bathroomHeaterTime = 0;
RCSwitch myRFOut = RCSwitch();
RCSwitch myRFIn = RCSwitch();


//FSR Stuff
const int pressureFrequency = 12000;        //12 secs (takes a 5 reading average)
const int pressureHistoryLength = 600;      //Number of pressure readings to record
int pressureHistory[pressureHistoryLength];
int pressureThreshold;                      //Point at which status changes from Up to Inbed
int lastPressure = 1024;
int highestPressure = 0;
int pressureSmoothing[5];
unsigned long lastPressureTime = millis();
unsigned long lastFlashTime = millis();
String status_FSR = "Up";


IRsend irsend1(PIN_IR_1);
IRsend irsend2(PIN_IR_2);
IRsend irsend3(PIN_IR_3);

String startTime = "";
int totalStartMinutes;             //Time in minutes since midnight that the server was started.  Set by getTimeNTC
int lastCalledMinutes;             //The last time a check was performed to make sure no time-dependent thing due
int nowMinutes;
int nowHours;
char sunset[6];
char sunrise[6];

const long interFlashtime=4500;    //Hard time in ms between runs of main loop (actual frequency is this plus code run time +500ms (for the Flash-on time)

unsigned long turnOffEncephalonAt = 0;

String password = "";
String scode = "";
uint8_t channel = 0;
uint32_t icode;



void setup() {
    //wdt_disable();
    wdt_enable(WDTO_8S);  //Set reset watchdog to 8 secs
    pinMode(0, OUTPUT);
    digitalWrite(0, LOW);
    pinMode(1, OUTPUT);
    digitalWrite(1, LOW);
    pinMode(3, OUTPUT);
    digitalWrite(3, LOW);
    pinMode(12, OUTPUT);
    digitalWrite(12, LOW);
    pinMode(13, OUTPUT);
    digitalWrite(13, LOW);

    pinMode(PIN_RELAY_1, OUTPUT);
    digitalWrite(PIN_RELAY_1, LOW);

    irsend1.begin();
    irsend2.begin();
    irsend3.begin();

    myRFIn.enableReceive(PIN_RF_IN);
    myRFOut.enableTransmit(PIN_RF_OUT);

    Serial.begin(9600);
    delay(10);
    WiFi.begin(ssid, wifipassword);
    Serial.println("Locating Wifi");

    // Wait for connection
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("");
    Serial.print("Connected to ");
    Serial.println(ssid);
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    if (mdns.begin("esp8266", WiFi.localIP())) {
        Serial.println("MDNS responder started");
    }

    EEPROM.begin(512);
    pulsewidth_r1 = EEPROM.read(eeprom_PW);
    Serial.print ("Setting pulsewidth_r1 to ");
    Serial.println (pulsewidth_r1);

    blanketDelay = EEPROM.read(eeprom_blanketDelay) * 12;
    if (blanketDelay == 0) {
        blanketDelay = 480;
    }
    blanketCooltemp = EEPROM.read(eeprom_blanketCooltemp);
    if (blanketCooltemp = 0) {
        blanketCooltemp = 2;
    }

    pressureThreshold = EEPROM.read(eeprom_PT) * 4;
    if (pressureThreshold == 0) {
        pressureThreshold = 330;
    }
    Serial.print ("Setting pressureThreshold to ");
    Serial.println (pressureThreshold);
    wdt_reset() ;
    getTimeFromNTP();
    wdt_reset() ;
    getSunTimes();

    server.on("/", handleRoot);
    server.on("/ir", handleIr);
    server.on("/rf", []() {
        if(getParams()) {
            doRF(icode);
        }
    });
    server.on("/r1", handleR1);
    server.on("/wol", handleWoL);
    server.on("/shutdown", handleShutdown);
    server.on("/blanket", []() {
        if (getParams()) {
            pulsewidth_r1 = icode;
            EEPROM.write(eeprom_PW, icode);
            EEPROM.commit();
            server.send(200, "text/plain", "OK-Setting blanket pulse width to " + scode);
            if (pulsewidth_r1 = 0) {
                blanketTime = -1;
            }
        }
    });

    server.on("/toasty20", []() {
        if (getParams()) {
            server.send(200, "text/plain", "OK-turning bathroom heater on for 20 mins. ");
            bathroomHeaterTime = 240; //Cycles of flash time
            doRF(11);
        }
    });

    server.on("/bedtime", []() {
        if (getParams()) {
            server.send(200, "text/plain", "OK-turning electric blanket on full for 40 mins then sleep mode. ");
            blanketTime = blanketDelay;                                //Cycles of flash time
            pulsewidth_r1 = 9;
        }
    });

    server.on("/reset", []() {
        server.send(200, "text/plain", "OK-Resetting in 8 secs");
        delay(8001);
    });
    
    server.on("/pressure", []() {
        String history = "";
        String smooth = "";

        for(int k = 0; k < 6; k++) {
            smooth = smooth + pressureSmoothing[k] + "|";
        }

        int countsPerMin = 60 / (pressureFrequency / 1000);
        for(int j = 0; j < pressureHistoryLength - countsPerMin; j = j + countsPerMin) {
            history = history + "<tr><td>";
            for(int k = 0; k < countsPerMin; k++) {
                history = history + pressureHistory[j + k] + "</td><td>";
            }
            history = history + pressureHistory[j + countsPerMin] + "</td></tr>";
        }

        char ptbuf[12];
        char pfbuf[12];
        itoa(pressureThreshold, ptbuf, 10);
        itoa(pressureFrequency / 1000, pfbuf, 10);

        server.send(200, "text/html", "<html><head><title>QueensRoad Controller</title></head><body>" + startTime + "+" + milliFormat() + ":<br>" + status_FSR + "<br>" + smooth + "<br>" + "Pressure Threshold: " + ptbuf + "<br>" + "Pressure Frequency: " + pfbuf + " secs<br><table border=1>" + history + "</table></body></html>");
    });
    
    server.on("/help", []() {
        server.send(200, "text/html", "<html><body><table><tr><td>ir</td><td>Send IR Code</td></tr><tr><td>rf</td><td>Send RF code</td></tr><tr><td>toasty20</td><td>Turn RF1 on for 20 mins</td></tr><tr><td>r1</td><td>Turn Relay 1 off/on</td></tr><tr><td>blanket</td><td>Alter pulsewidth of R1</td></tr><tr><td>setblankettime</td><td>Time in minutes blanket says at PW9</td></tr><tr><td>setblanketct</td><td>PW blanket goes to after initial warmup time</td></tr> <tr><td>bedtime</td><td>Activate electric blanket</td></tr><tr><td>pressure</td><td>Get Pressure History</td></tr><tr><td>setpt</td><td>Set Pressure Threshold</td></tr><tr><td>pin</td><td>Toggle a pin</td></tr><tr><td>wol</td><td>Wake Encephalon</td></tr><tr><td>shutdown</td><td>Shutdown Encephalon</td></tr><tr><td>reset</td><td>Reset server</td></tr></table></body></html>");
    });
    
    server.on("/pin", []() {
        if (getParams()) {
            digitalWrite(channel, icode);
            server.send(200, "text/plain", "OK");
        }

    });
    server.on("/setpt", []() {
        if (getParams()) {
            pressureThreshold=icode;
            EEPROM.write(eeprom_PT, icode / 4);
            EEPROM.commit();
            server.send(200, "text/plain", "Set pressure threshold OK");
        }
    });

    server.on("/setblankettime", []() {
        if (getParams()) {
            blanketDelay = icode * 40;
            EEPROM.write(eeprom_blanketDelay, icode);
            EEPROM.commit();
            server.send(200, "text/plain", "Set Blanket Delay Time OK");
        }
    });

    server.on("/setblanketct", []() {
        if (getParams()) {
            blanketCooltemp = icode;
            EEPROM.write(eeprom_blanketCooltemp, icode);
            EEPROM.commit();
            server.send(200, "text/plain", "Set Blanket Snooze Temp OK");
        }
    });

   
    server.onNotFound(handleNotFound);

    // Start the server
    server.begin();
    Serial.print("HTTP server started on ");
    Serial.println(WiFi.localIP());
    Serial.print("UDP server started on port ");
    Serial.println(WOL_PORT);
    udp.begin(WOL_PORT);
    
}


void handleRoot() {
    char buf[12];
    char buf2[12];
    char buf3[12];
    char buf4[12];
    char buf5[12];
    itoa(pulsewidth_r1, buf, 10);
    itoa(bathroomHeaterTime, buf2, 10);
    itoa(blanketTime, buf3, 10);
    itoa(nowMinutes,buf4,10);
    itoa(nowHours,buf5,10);
    server.send(200, "text/html",
                "<html><head><title>QueensRoad Controller</title></head><body><form action='?' method='get'>"\
                "<p>Server started at " + startTime + "</p><p>Now: " + buf5+":"+buf4+ "</p>" \
                "<p><a href=\"wol\">Turn on Archie</a></p>"
                "<p><a href=\"r1?code=1\">Turn relay on</a></p>" \
                "<p><a href=\"r1?code=0\">Turn relay off</a></p>" \
                "<p>Relay status: " + status_relay + "</p>" \
                "<p>Relay pulse width: " + buf + "</p><br>" \
                "<p><a href=\"ir?code=16712445&channel=1\">Turn upstairs LED on/off</a></p>" \
                "<p><a href=\"ir?code=16712445&channel=2\">Turn downstairs LED on/off</a></p><br>" \
                "<p>Suspected RF status: " + status_RF[1] + "/" + status_RF[2] + "/" + status_RF[3] + "/" + status_RF[4] + "/" + status_RF[5] + "/" + status_RF[6] + "/" + status_RF[7] + "</p>" \
                "<p>Bathroom heater countdown: " + buf2 + "</p>" \
                "<p>Electric blanket countdown: " + buf3 + "</p>" \
                "</form></body></html>");
}


void loop() {
    
    //Serial.println (analogRead(0));
    if (myRFIn.available()) {
        Serial.print("RF Detected: (Value");
        Serial.print(myRFIn.getReceivedValue());
        Serial.print(" Bitlength:");
        Serial.print (myRFIn.getReceivedBitlength());
        Serial.print(" Delay:");
        Serial.print(myRFIn.getReceivedDelay());
        Serial.print(" Protocol:");
        Serial.println(myRFIn.getReceivedProtocol());

        if (myRFIn.getReceivedValue() == 2071561683) {
            sendToNexus("doorbell");
        }
        myRFIn.resetAvailable();
    }

    if (lastFlashTime + interFlashtime < millis()) {
        digitalWrite(LED_BUILTIN, LOW);   // turn the LED on (HIGH is the voltage level - inverted because builtin LED has a pull-up resistor)
        delay(500);                       // wait for half a second
        digitalWrite(LED_BUILTIN, HIGH);
        lastFlashTime = millis();
     
        bathroomHeaterTime = bathroomHeaterTime - 1;
        if (bathroomHeaterTime == 0) {
            doRF(10);
        }
        if (bathroomHeaterTime == -1) {
            bathroomHeaterTime = 0;
        }

        //This is used for the bedtime countdown (ie turn blanket down once bed prewarmed)

        blanketTime = blanketTime - 1;
        if (blanketTime == 0) {
            pulsewidth_r1 = blanketCooltemp;
        }
        if (blanketTime <0 ) {
            blanketTime = 0;
        }
        doPWM();
        
        int nowTotalTimeMinutes=totalStartMinutes+(millis()/60000);
        nowMinutes=nowTotalTimeMinutes%60;
        if (lastCalledMinutes!=nowMinutes){
          lastCalledMinutes=nowMinutes;
          nowHours=(nowTotalTimeMinutes/60)%24;
          char nowTime [6];
          sprintf (nowTime, "%02d:%02d",nowHours, nowMinutes);
          
          
          if (strcmp(nowTime,sunrise)==0){   //Because of COURSE strcmp works on char arrays :-)
            Serial.print ("Toggling porch LED");
            irsend2.sendNEC(16712445, 32);//Turn on
          }
          if (strcmp(nowTime,sunset)==0){   //Because of COURSE strcmp works on char arrays :-)
            Serial.print ("Toggling porch LED");
            irsend2.sendNEC(16712445, 32);//Turn on
          }
          
          int nowDigits=nowMinutes%10;
         
          if (nowDigits==0){
              //Call Salus Schedule every 10 mins
                
              Serial.print (nowHours);
              Serial.print (":");
              Serial.print (nowMinutes);
        
              Serial.println(" Running Scheduler");
              //Probably easier to code any other scheduled stuff within schedule.php
              WiFiClient client;
    
              if (client.connect("home.jonbray.net", 80)) {
                // Make a HTTP request:
                client.println("GET /curl/schedule.php HTTP/1.1");
                client.println("Host: home.jonbray.net");
                client.println();
                
                if (client.available()) {
                    char c = client.read();
                    Serial.print(c);
                }
                  
              }
          }
        
        }
    }

    server.handleClient();

    if (lastPressureTime + pressureFrequency < millis()) {
        lastPressureTime = millis();
        checkBedPressure();
    }
}

boolean getParams() {
    for (uint8_t i = 0; i < server.args(); i++) {
        if (server.argName(i) == "password") {
            password = server.arg(i).c_str();
        }
        if (server.argName(i) == "channel") {
            channel = strtoul(server.arg(i).c_str(), NULL, 10);
        }
        if (server.argName(i) == "code") {
            icode = strtoul(server.arg(i).c_str(), NULL, 10);
            scode = server.arg(i).c_str();
        }
    }
    if (password != "monowire") {
        server.send(401, "text/plain", "Invalid password");
        return 0;
    } else {
        return -1;
    }
}

void handleWoL() {
    //if (getParams()){
    byte preamble[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    byte i;
    doRF(51);           //Apply power to the computer socket
    delay(2000);
    udp.beginPacket(wol_ip, WOL_PORT);
    udp.write(preamble, sizeof preamble);
    for (i = 0; i < 16; i++)
        udp.write(mac, sizeof mac);

    udp.endPacket();
    server.send(200, "text/plain", "WOL Packet Sent");
    irsend1.sendNEC(16712445, 32);//Turn on
    irsend1.sendNEC(16759365, 32);//Green
    // }
}
void handleShutdown() {
  
    WiFiClient client;
    irsend1.sendNEC(16726725, 32);                                //Red
    if (client.connect("jongbray.routemehome.com", 80)) {
        // Make a HTTP request:
        client.println("GET /index.html?turnoffnow HTTP/1.1");
        client.println("Host: jongbray.routemehome.com");
        client.println();
        if (client.available()) {
            char c = client.read();
            Serial.print(c);
        }
    }

    server.send(401, "text/plain", "Shutdown command sent OK");
    delay(7000);
    wdt_reset();
    delay(7000);
    wdt_reset();
    doRF(50);        //Power off the RF socket
    server.send(401, "text/plain", "RF shutdown sent OK");
    irsend1.sendNEC(16712445, 32);
}

void handleIr() {
    if (getParams()) {
        switch (channel) {
        case 2: {
            irsend2.sendNEC(icode, 32);
            break;
        }
        case 3: {
            irsend3.sendNEC(icode, 32);
            break;
        }
        case 1: {
        }
        default: {
            irsend1.sendNEC(icode, 32);
            break;
        }
        }
    }
    server.send(401, "text/plain", "IR message sent OK");
}

void handleR1() {
    if(getParams()) {
        if (scode == "1") {
            digitalWrite(PIN_RELAY_1, HIGH);
            pulsewidth_r1 = 9;
            server.send(401, "text/plain", "Setting R1 on");
            status_relay = "ON";
        } else {
            digitalWrite(PIN_RELAY_1, LOW);
            pulsewidth_r1 = 0;
            blanketTime = -1;
            server.send(401, "text/plain", "Setting R1 off");
            status_relay = "OFF";
        }
    }
}

void doPWM() {
    static int pwmStage;
    pwmStage++;
    if (pwmStage > 9) {
        pwmStage = 0;
    }
    //Serial.print (pwmStage);
    if (pwmStage >= pulsewidth_r1) {
        digitalWrite(PIN_RELAY_1, LOW);
        //Serial.println ("Turning R1 Off");
    } else {
        digitalWrite(PIN_RELAY_1, HIGH);
        //Serial.println ("Turning R1 On");
    }
}

void doRF(int icode) {
    int channel = (icode / 10);
    int bOn = (icode % 10);

    if (channel < 5) {
        myRFOut.setProtocol(6);
        myRFOut.setPulseLength(666);
    } else {
        myRFOut.setProtocol(1);
        myRFOut.setPulseLength(155);
    }
    switch (bOn) {
    case 1: {
        myRFOut.send(on_RF[channel - 1]);
        status_RF[channel] = "ON";
        break;
    }
    case 0: {
        myRFOut.send(off_RF[channel - 1]);
        status_RF[channel] = "OFF";
        break;
    }
    }
    Serial.print ("Turning RF ");
    Serial.print (channel);
    Serial.print (" ");
    Serial.println (status_RF[channel]);
    server.send(401, "text/plain", "RF status now " + status_RF[1] + "/" + status_RF[2] + "/" + status_RF[3] + "/" + status_RF[4] + "/" + status_RF[5] + "/" + status_RF[6] + "/" + status_RF[7]);
}

void handleNotFound() {
    String message = "File Not Found\n\n";
    message += "URI: ";
    message += server.uri();
    message += "\nMethod: ";
    message += (server.method() == HTTP_GET) ? "GET" : "POST";
    message += "\nArguments: ";
    message += server.args();
    message += "\n";
    for (uint8_t i = 0; i < server.args(); i++)
        message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
    server.send(404, "text/plain", message);
}

void checkBedPressure() {
    static int smoothingPosition;    //Last 0-5 measurements; averages and resets when 5 hit.
    static int historyPosition;      //Last [pressureHistoryLength] averages
    static int pressureReadingOld;
    int pressureReading = 0;
    smoothingPosition++;
    pressureSmoothing[smoothingPosition] = analogRead(0);
    if (smoothingPosition < 5) {
        return;
    }
    smoothingPosition = 0;

    pressureThreshold = EEPROM.read(eeprom_PT) * 4;
  
    for (int j = 1; j < 6; j++) {
        pressureReading = pressureReading + pressureSmoothing[j];
    }
    pressureReading = pressureReading / 5;
    
    historyPosition++;
    if(historyPosition == pressureHistoryLength) {
        //Clear pressure history if limit exceeded
        historyPosition = 0;
        String dump="";
        
        for(int i = 0; i < pressureHistoryLength; i++) {
            char pH[12];
            itoa(pressureHistory[i], pH, 10);
            dump=dump+pH+"|";
            pressureHistory[i] = 0;
        }
         WiFiClient client;
        if (client.connect("home.jonbray.net", 80)) {
          // Make a HTTP request:
          client.println("GET /curl/uploadpressure.php?history="+dump+" HTTP/1.1");
          client.println("Host: home.jonbray.net");
          client.println();
          
          if (client.available()) {
              char c = client.read();
              Serial.print(c);
          }
            
        }
    }

    char pR[5];
    itoa(pressureReading, pR, 10);
    char mS[14];
    itoa(millis(), mS, 10);
    Serial.print ("Pressure Reading: ");
    Serial.print (pressureReading);
    Serial.print("Threshold:");
    Serial.println(pressureThreshold);
    
    if ((pressureReading > pressureThreshold) && (lastPressure <= pressureThreshold)) {
        Serial.println(" Into bed");
        status_FSR = "inbed";
        pressureHistory[historyPosition] = 1024;
        historyPosition++;
        String sTemp = "bedstatus|inbed|" ;
        sTemp = sTemp + pressureReading + "|";
        sTemp = sTemp + millis();
        sendToNexus(sTemp);
    }

    if ((pressureReading <= pressureThreshold ) && (lastPressure > pressureThreshold)) {
        Serial.println(" Out of bed");
        status_FSR = "outofbed";
        String sTemp = "bedstatus|outofbed|" ;
        sTemp = sTemp + pressureReading + "|";
        pulsewidth_r1 = 0;        //Turn off electric blanket
        sTemp = sTemp + millis();
        sendToNexus(sTemp);
    }

    lastPressure = pressureReading;
    pressureHistory[historyPosition] = pressureReading;

}


void sendToNexus(String message) {

    WiFiClient client;

    if (client.connect("autoremotejoaomgcd.appspot.com", 80)) {
        // Make a HTTP request:
        Serial.println("Sending " + message);
        client.println("GET /sendmessage?key=APA91bH6zCF2KSqvh-JFLvhM_W40sDdOnwk6JnaEEhSbQ5UVdbMTBHICYaSI-VIssRIQ72WIJkVOl7hZ1ujA1Q5V3okFX1d3kIeaFBlY6Vmh2oDG5gcF_6qoSuehn2NnOXRbwF78BUEs&message=" + message + " HTTP/1.1");
        client.println("Host: autoremotejoaomgcd.appspot.com");
        client.println();
        if (client.available()) {
            char c = client.read();
            Serial.print(c);
        }
    }
}

String webGet(String site, String query){
  
    WiFiClient client;
    
    int n = site.length(); 
    // declaring character array
    char char_site[n+1]; 
    // copying the contents of the string to char array
    strcpy(char_site, site.c_str()); 
    
    if (client.connect(char_site, 80)) {
        // Make a HTTP request:
        client.println("GET /"+query+ " HTTP/1.1");
        client.println("Host: "+site);
        client.println();
        if (client.available()) {
            String line = client.readStringUntil('\r');
            return(line);
        }
    }
}


void getTimeFromNTP() {
    WiFiClient client;

    int ln = 0;
    int httpPort = 13;

    if (!client.connect(ntphost, httpPort)) {
        Serial.println("NTP connection failed");
        return;
    }

    // This will send the request to the server
    client.print("HEAD / HTTP/1.1\r\nAccept: */*\r\nUser-Agent: Mozilla/4.0 (compatible; ESP8266 NodeMcu Lua;)\r\n\r\n");

    delay(100);

    // Read all the lines of the reply from server and print them to Serial
    // expected line is like : Date: Thu, 01 Jan 2015 22:00:14 GMT
    char buffer[12];
    String dateTime = "";
    String TimeDate;

    while(client.available()) {
        String line = client.readStringUntil('\r');

        if (line.indexOf("Date") != -1) {
            Serial.print("=====>");
        } else {
            // Serial.print(line);
            // date starts at pos 7
            TimeDate = line.substring(7);
            Serial.println(TimeDate);
            // time starts at pos 14
            TimeDate = line.substring(7, 15);
            TimeDate.toCharArray(buffer, 10);
            TimeDate = line.substring(16, 24);
            TimeDate.toCharArray(buffer, 10);
        }
    }
    startTime = TimeDate;
    String sstartHours=startTime.substring(0,2);
    String sstartMinutes=startTime.substring(3,5);
    int startHours=sstartHours.toInt();
    int startMinutes=sstartMinutes.toInt();
    totalStartMinutes=(startHours*60);
    totalStartMinutes=totalStartMinutes+startMinutes;
    Serial.print("Start @ ");
    Serial.print(startHours);
    Serial.print(":");
    Serial.println(startMinutes);
}

void getSunTimes(){
  WiFiClient client;
  if (client.connect("home.jonbray.net", 80)) {
        // Make a HTTP request:
        client.println("GET /curl/getsuntimes.php HTTP/1.1");
        client.println("Host: home.jonbray.net");
        client.println("Connection: close");
        client.println();
        delay(2000);

        while(client.available()) {
          String line = client.readStringUntil('\r');

          if (line.indexOf("TIMES") != -1) {
              //Because there is a LOAD of shit in the header we can't be arsed with
              String ssunrise=line.substring(8,13);
              ssunrise.toCharArray(sunrise,6);
              
              String ssunset=line.substring(14,19);
              ssunset.toCharArray(sunset,6);
             
              Serial.print("Sunrise:");
              Serial.println(sunrise);
              Serial.print("Sunset:");
              Serial.println(sunset);    
          }
        
        }
        client.stop();
  }
}

String milliFormat() {
    unsigned long minutes = millis() / 60000;
    unsigned long hours = minutes / 60;
    minutes = minutes - (hours * 60);
    char sM[5];
    char sH[10];
    sprintf(sM, "%dm", minutes);
    sprintf(sH, "%dh ", hours);
    strcat(sH, sM);
    return sH;
}


