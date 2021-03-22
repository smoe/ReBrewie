#include <OneWire.h>
#include <DallasTemperature.h>
#include <SPI.h>
//#include <Wire.h>

//#define B20

#include "Brewie.h"
#include "Pump.h"
#include "Heater.h"
#include "Valves.h"

// Brewie Version setup
#include "B20Plus.h"
//#include "B20.h"

// Global Configurations
const bool externalCooling = false;       // Sets the cooling step to run wort out the outlet to an external wort chiller, otherwise default chill setup
const bool trueSparge = false;            // If set, dump mash liquor into external container before sparging with pure water (increase extraction). Manually return to boil tank
bool whirlpoolStep = false;               // If set (cooling temp > 70C), whirlpool for set time before cooling down to ~20C
const uint16_t whirlpoolCoolTemp = 200;   // Cool temperature after a whirlpool step, temp in C x 10
const uint16_t whirlpoolTime = 1800;      // Default whirlpool time: 30 minutes
const uint8_t spargePumpSpeed = 100;      // Reduce sparging pumping speed
const uint8_t mashingPumpSpeed = 70;      // Mashing pump speed for timed steps
const uint8_t heatingPumpSpeed = 120;     // Mashing pump speed for heating steps

const uint8_t mcuVersion = 7;

// Brewie Command variables
char brewieData[120];                  // Raw data from Olimex
char brewieCommand[24][2];             // Store received information position and length
char errorMessage[4][5];               // Error message buffer to report back to Olimex
char brewieMessage[4][5];              // Standard message buffer to report commands to Olimex
char echoMessage[120];                 // Echo buffer for echoing issued commands back to Olimex
uint8_t errorCount = 0;                   // Count variables if multiple commands or errors arise
uint8_t messageCount = 0;
uint8_t echoCount = 0;

// Flags and indicators
bool powerOn = false;
bool calibrationSent = false;
bool newStep = false;
bool newCommand = false;
bool brewiePause = false;
bool stepActive = false;
bool spargeOverride = false;
bool brewieWaterFill = false;
bool waterFilled = false;
bool brewing = false;
bool fanOverride = false;
bool safetyShutdown = false;
bool errorOverride = true;
bool waterOverride = false;
bool fastWaterReadings = false;
bool brewieStepRequestFlag = false;
bool requestNextStep = false;
uint8_t stepChangeState = 0;
uint32_t stepChangeTime = 0;

// MCU step variables
int16_t brewieStepBuffer[22];
int16_t brewieStep[22];
int8_t currStep = -1;
uint16_t totalTime = 0;
uint16_t stepTime = 0;
int16_t leftTime = 0;
bool lastStep = false;

// Calibration to be loaded by P80 command
float toLiter = 266.000;
float toLiterNull = 2515.0;
float mashDelta = 0.0;
float boilDelta = 0.0;
float pressureTempCal = 18.0;

// One Wire Temperature Sensors (2 for B20+, 3 for B20)
OneWire* MashTempPin;
OneWire* BoilTempPin;
DallasTemperature* MashTemp;
DallasTemperature* BoilTemp;
DeviceAddress mashAddress;
DeviceAddress boilAddress;
#ifdef B20
OneWire ChassisTempPin(CHASSIS_TEMP);
DallasTemperature BoilTemp(&ChassisTempPin);
DeviceAddress chassisAddress;
#endif

// Sensor Variables
uint8_t massAddress = 25;             // Default for my machine
uint16_t massSensor = 0;
int16_t massFast = 0;
int16_t massOffset = 0;
bool validMass = false;
float waterVolume = 0;
uint16_t massTemp = 0;
float massTempInC = 0.0;
float acCurrent = 0;
float acPeakCurrent = 0;
float acMeasure  = 0;
float machineVoltage = 0;
float boardTemp = 0;
float inlet1A = 0;
float servosA = 0;
float boilPumpA = 460;
float mashPumpA = 480;
float pumpTemp = 0;
uint16_t waterInletTemp = 692;        // From Mass Sensor (default ~18C)

float mashTankVolume = 0.0;
float boilTankVolume = 0.0;

// Heater Control Variables (to pass to Heater Object)
float mashTemp = 0.00;
float boilTemp = 0.00;

uint32_t statusTime = millis();
uint32_t waterTime = millis();
uint32_t leftOverTime = 0;

// Interrupt Variables
volatile uint8_t mashTicks = 0;
volatile uint8_t boilTicks = 0;
volatile bool powerButton = false;
volatile bool drainButton = false;

Brewie* brewie;
Pump* mashPump;
Pump* boilPump;

void setup() {
  Initialize_2560();
}

void loop() {
  if (powerOn) {
    Process_Sensors();
    brewie->Temperature_Control();
    if (digitalRead(POWER_BUTTON) == HIGH) {
      powerButton = true;
    }
    if (Brewie_Read() == false) {
      // If no command over UART, Process
      Fill_Boil_Tank();
      if (stepTime > 1) {
        Step_Complete();
      }
      Run_Sparge();
      Fast_Water_Readings();
    }
  } else {
    if (digitalRead(POWER_BUTTON) == HIGH) {
      if (drainButton) {
        // Set whirlpool mode
        //whirlpoolStep = true;
      } else {
        //whirlpoolStep = false;
      }
      Power_On();
    }
    Brewie_Read();
  }
  Brewie_Status();
}

bool Brewie_Read() {
  static uint8_t charCount;
  static uint32_t serialTimeout;
  static uint8_t fieldPos;
  static uint8_t fieldLen;
  static bool incomingCommand = false;

  if (incomingCommand) {
    if ((millis() - serialTimeout) > 100 && incomingCommand) {
      incomingCommand = false;
    } 
    if (Serial.available()) {
      brewieData[charCount] = Serial.peek();
      serialTimeout = millis();
      if (brewieData[charCount] == 0x20) {
        Serial.read();
        fieldPos++;
        brewieCommand[fieldPos][0] = charCount+1;
        brewieCommand[fieldPos-1][1] = fieldLen;
        fieldLen = 0;
      } else {
        Serial.read();
        fieldLen++;
      }
      if (charCount-2 > brewieData[1]) {
        if (brewieData[charCount] == 0x2A) {
          //Serial.println("end char received");
          brewieCommand[0][0] = fieldPos;
          brewieCommand[fieldPos][1] = fieldLen-2;
          brewieData[charCount-1] = 0x00;
          incomingCommand = false;
          newCommand = true;
          Brewie_ACK_Write();
          Brewie_Command_Decode();
        }
      }
      if (charCount > 119) {
        incomingCommand = false;
      }
      charCount++;
    }
  } else if (Serial.available()) {
    if (Serial.read() == '$') {
      incomingCommand = true;
      serialTimeout = millis();
      // Command is being sent, extract data
      while(Serial.available() < 2) {
        if (millis() - serialTimeout > 100) {
          break;
        }
      }
      brewieData[0] = Serial.read();  // Command number
      brewieData[1] = Serial.read();  // Command length
      charCount = 2;
      fieldPos = 0;
      fieldLen = 0;
      for (uint8_t i = 0; i<24; i++) {
        brewieCommand[i][0] = 0;
        brewieCommand[i][1] = 0;
      }
    } else {
      Serial.read();
    }
  }

  return incomingCommand;
}

void Brewie_ACK_Write() {
  Serial.print("$");
  Serial.write(1);
  Serial.write(brewieData[0]);
  Serial.write(0x2A);
  Serial.write(0x0D);
  Serial.write(0x0A);
}

void Brewie_Status_Write() {
  // Build up status string
  uint8_t brewieLength = 0;
  char statusMessage[255];
  
  if (currStep == -1) {
    brewieLength  = sprintf(statusMessage, "%d\t%u\t\t\t", currStep, totalTime);
  } else {
    brewieLength  = sprintf(statusMessage, "%d\t%u\t%u\t%u\t", currStep, totalTime, stepTime, leftTime);
  }
  for(uint8_t x = 0; x < messageCount; x++) {
    brewieLength += sprintf(&statusMessage[brewieLength], "%s,", brewieMessage[x]);
  }
  for(uint8_t x = 0; x < errorCount; x++) {
    brewieLength += sprintf(&statusMessage[brewieLength], "%s,", errorMessage[x]);
  }
  brewieLength += sprintf(&statusMessage[brewieLength], "V%d\t", mcuVersion);
  brewieLength += sprintf(&statusMessage[brewieLength], "%u\t", (uint16_t)massSensor);
  brewieLength += floatToStringAppend(&waterVolume, &statusMessage[brewieLength]);
  float floatTemp = mashTemp - mashDelta;
  brewieLength += floatToStringAppend(&floatTemp, &statusMessage[brewieLength]);
  floatTemp = boilTemp - boilDelta;
  brewieLength += floatToStringAppend(&floatTemp, &statusMessage[brewieLength]);
  floatTemp = mashTemp*10.0;
  brewieLength += floatToStringAppend(&floatTemp, &statusMessage[brewieLength]);
  floatTemp = boilTemp*10.0;
  brewieLength += floatToStringAppend(&floatTemp, &statusMessage[brewieLength]);
  floatTemp = (float)(mashPump->pumpTach());
  brewieLength += floatToStringAppend(&floatTemp, &statusMessage[brewieLength]);
  floatTemp = (float)(boilPump->pumpTach());
  brewieLength += floatToStringAppend(&floatTemp, &statusMessage[brewieLength]);
  brewieLength += sprintf(&statusMessage[brewieLength], "%u\t\%u\t%u\t\%u\t", mashPump->pumpDiag(), (uint16_t)mashPumpA, boilPump->pumpDiag(), (uint16_t)boilPumpA);
  brewieLength += sprintf(&statusMessage[brewieLength], "%u\t\%u\t%u\t", (uint8_t)brewie->MashHeaterOn(), (uint8_t)brewie->BoilHeaterOn(), (uint16_t)boardTemp);
  if (newCommand) {
    brewieLength += sprintf(&statusMessage[brewieLength], echoMessage);
  }
  brewieLength += sprintf(&statusMessage[brewieLength], "\t");
  floatTemp = acMeasure;
  brewieLength += floatToStringAppend(&floatTemp, &statusMessage[brewieLength]);
  brewieLength += sprintf(&statusMessage[brewieLength], "\t");
  floatTemp = massTempInC;
  brewieLength += floatToStringAppend(&floatTemp, &statusMessage[brewieLength]);
  brewieLength += sprintf(&statusMessage[brewieLength], "\t%d", massOffset);

  brewieLength += sprintf(&statusMessage[brewieLength], "\r\n");
  
  Serial.print(statusMessage);
}

void Brewie_Command_Decode() {
  static uint8_t hopTanksOpen = 0;
  if (echoCount > 0) {
    echoCount += sprintf(&echoMessage[echoCount], ",");
  }
  echoCount += sprintf(&echoMessage[echoCount], &brewieData[2]);
  
  if (brewieData[2] == 'P') {
    // Last two digits of command 
    uint8_t commandNum = (brewieData[4]-0x30)*10+(brewieData[5]-0x30);
  
    if (brewieData[3] == '1') {
      if (commandNum == 3) {
        Brewie_Step();
      } else if (commandNum == 10) {
        digitalWrite(INLET_1, HIGH);
      } else if (commandNum == 11) {
        digitalWrite(INLET_1, LOW);
      } else if (commandNum == 24) {
        mashPump->setPumpSpeed(255);
      } else if (commandNum == 25) {
        mashPump->setPumpSpeed(0);
      } else if (commandNum == 26) {
        boilPump->setPumpSpeed(255);
      } else if (commandNum == 27) {
        boilPump->setPumpSpeed(0);
      } else if (commandNum == 28) {
        digitalWrite(INLET_2, HIGH);
      } else if (commandNum == 29) {
        digitalWrite(INLET_2, LOW);
      } else if (commandNum == 12) {
        setValve(VALVE_MASH_IN, VALVE_OPEN);
      } else if (commandNum == 13) {
        setValve(VALVE_MASH_IN, VALVE_CLOSE);
      } else if (commandNum == 14) {
        setValve(VALVE_BOIL_IN, VALVE_OPEN);
      } else if (commandNum == 15) {
        setValve(VALVE_BOIL_IN, VALVE_CLOSE);
      } else if (commandNum == 16) {
        setValve(VALVE_HOP_1, VALVE_OPEN);
        hopTanksOpen++;
      } else if (commandNum == 17) {
        setValve(VALVE_HOP_1, VALVE_CLOSE);
        hopTanksOpen--;
      } else if (commandNum == 18) {
        setValve(VALVE_HOP_2, VALVE_OPEN);
        hopTanksOpen++;
      } else if (commandNum == 19) {
        setValve(VALVE_HOP_2, VALVE_CLOSE);
        hopTanksOpen--;
      } else if (commandNum == 20) {
        setValve(VALVE_HOP_3, VALVE_OPEN);
        hopTanksOpen++;
      } else if (commandNum == 21) {
        setValve(VALVE_HOP_3, VALVE_CLOSE);
        hopTanksOpen--;
      } else if (commandNum == 22) {
        setValve(VALVE_HOP_4, VALVE_OPEN);
        hopTanksOpen++;
      } else if (commandNum == 23) {
        setValve(VALVE_HOP_4, VALVE_CLOSE);
        hopTanksOpen--;
      } else if (commandNum == 30) {
        setValve(VALVE_COOL, VALVE_OPEN);
      } else if (commandNum == 31) {
        setValve(VALVE_COOL, VALVE_CLOSE);
      } else if (commandNum == 32) {
        setValve(VALVE_OUTLET, VALVE_OPEN);
      } else if (commandNum == 33) {
        setValve(VALVE_OUTLET, VALVE_CLOSE);
      } else if (commandNum == 34) {
        setValve(VALVE_MASH_RET, VALVE_OPEN);
      } else if (commandNum == 35) {
        setValve(VALVE_MASH_RET, VALVE_CLOSE);
      } else if (commandNum == 36) {
        setValve(VALVE_BOIL_RET, VALVE_OPEN);
      } else if (commandNum == 37) {
        setValve(VALVE_BOIL_RET, VALVE_CLOSE);
      } else if (commandNum == 50) {
        brewie->setTemperatures(atof(&brewieData[brewieCommand[1][0]])/10.0, -1.0);
        safetyShutdown = false;
      } else if (commandNum == 51) {
        brewie->setTemperatures(-1.0, atof(&brewieData[brewieCommand[1][0]])/10.0);
        safetyShutdown = false;
      }
    } else if (brewieData[3] == '2') {
      if (commandNum == 0) {            // Start
        //statusTime = millis();
        totalTime = 0;
        brewing = true;
        acMeasure = 0;
        brewieStepRequestFlag = true;
        digitalWrite(VENT_FAN, HIGH);
        digitalWrite(POWER_FAN, HIGH);
        brewie->Start();
      } else if (commandNum == 1) {     // Pause
        Brewie_Pause();
      } else if (commandNum == 2) {     // Continue
        brewing = true;
        brewiePause = false;
        if (safetyShutdown) {
          // Possible error has occured, let user bypass
          errorOverride = true;
          waterOverride = true;
        }
        safetyShutdown = false;
        Process_Step();
        digitalWrite(VENT_FAN, HIGH);
        digitalWrite(POWER_FAN, HIGH);
      } else if (commandNum == 4) {     // Next step
        if (brewieStepRequestFlag == false || lastStep == true) {
          requestNextStep = true;
          stepActive = false;
          brewieWaterFill = false;
          spargeOverride = false;
        }
      } else if (commandNum == 5) {     // Developer Mode/Fast water readings
        if (brewieData[brewieCommand[1][0]] == '1') {
          //brewing = true;
          powerOn = true;
          fanOverride = true;
          fastWaterReadings = true;
          digitalWrite(VENT_FAN, HIGH);
          digitalWrite(POWER_FAN, HIGH);
        } else {
          fanOverride = false;
          brewing = false;
          fastWaterReadings = false;
        }
      }
    } else if (brewieData[3] == '8') {
      // Load calibration constants to MCU
      calibrationSent = true;
      digitalWrite(POWER_LIGHT, HIGH);
      toLiter = atof(&brewieData[brewieCommand[1][0]]);
      toLiterNull = atof(&brewieData[brewieCommand[2][0]]);
      mashDelta = atof(&brewieData[brewieCommand[3][0]]);
      boilDelta = atof(&brewieData[brewieCommand[4][0]]);
      Power_On();
      Close_All_Valves();
      Serial.print("!Scanning I2C...");
      uint8_t iAddress = ScanTWI();
      Serial.println("done");
      if (iAddress > 1 && iAddress < 128) {
        //massAddress = iAddress; 
        Serial.print("!Pressure sensor found at address: 0x");
        Serial.println(iAddress, HEX);
      } else {
        Serial.println("!Pressure sensor not found!");
      }
      Brewie_Reset();
    } else if (brewieData[3] == '9') {
      if (commandNum == 98) {
        if (currStep != -1) {
          if (brewieCommand[0][0] == 0) {
            brewiePause = true; 
            Close_All_Valves();
            hopTanksOpen = 0;
          } 
        } 
        if (brewieData[brewieCommand[1][0]] == '1') {
          digitalWrite(DRAIN_LIGHT, HIGH);
        } else if (brewieData[brewieCommand[1][0]] == '0') {
          digitalWrite(DRAIN_LIGHT, LOW);
        }
      }
      if (commandNum == 99) {
        if (brewieCommand[0][0] == 1) {
          Power_Off();
        } else {
          Close_All_Valves();
          Brewie_Reset();
          hopTanksOpen = 0;
        }
      }
    }
  }

  if (boilPump->isRunning()) {
    if (hopTanksOpen > 0) {
      boilPump->setPumpSpeed(80+50*(hopTanksOpen-1));
    } else if (hopTanksOpen > 4) {
      hopTanksOpen = 4;
    }
  }
}

uint8_t floatToStringAppend(float* data, char* str) {
  dtostrf(*data, 6, 6, str);
  char start = 6;
  start += sprintf(&str[start], "\t");
  return start;
}

bool setValve(uint8_t valve, uint8_t angle) {
  uint16_t setAngle = 0;

  // Use 0 and 1 to mean closed and open, but also allow for custom angles for higher values
  if (angle == 0) {
    // Valve Closed 
    setAngle = VALVE_CLOSE_ANGLE; 
  } else if (angle == 1) {
    setAngle = VALVE_OPEN_ANGLE;
  } else if (angle <= VALVE_CLOSE_ANGLE) {
    setAngle = angle;
  } else if (angle == 255) {
    setAngle = VALVE_OPEN_ANGLE;
  } else {
    Serial.println("!Bad Valve Setpoint");
  }

  if (valveState[valve] != setAngle) {
    valvePWM = valve;
    OCR4A = 1000 + (uint16_t)(setAngle*17);
    digitalWrite(PWR_EN_SERVO, HIGH);
    TIMSK4 |= _BV(TOIE4) + _BV(OCIE4A);
    uint32_t servoTime = millis();
    uint16_t valveSum = 0;
    while(millis() - servoTime < 600) {
      for (int x = 0; x < 64; x++) {
        valveSum += analogRead(I_VALVES);
      }
      uint16_t valveI = valveSum/64;
      valveSum = 0;
      if ((millis() - servoTime > 120) && (valveI < 35)) {
        break;
      }
    }
    delay(50);
    digitalWrite(PWR_EN_SERVO, LOW);
    TIMSK4 &= ~(_BV(TOIE4) + _BV(OCIE4A));
    valveState[valve] = setAngle;
  }
  return valveState[valve] > VALVE_CLOSE_ANGLE;
}

void Brewie_Step() {
  if (brewieStepBuffer[0] == -1) {
    for (uint8_t go = 0; go < 22; go++) {
      brewieStepBuffer[go] = 0;
    }
    for (uint8_t commands = 1; commands <= brewieCommand[0][0]; commands++) {
      for (uint8_t len = 0; len < brewieCommand[commands][1]; len++) {
        brewieStepBuffer[commands-1] += brewieData[brewieCommand[commands][0]+len] - 0x30;
        if (brewieCommand[commands][1] - len > 1) {
          brewieStepBuffer[commands-1] *= 10;
        }
      }
    }
    newStep = true;
    // Signal that a new step was received from the previous step request
    brewieStepRequestFlag = false;
  } else {
    Serial.println("Step Buffer Error");
  }
}

void Step_Change() {
  if (brewing) {
    if (!stepActive && newStep) {
      if (brewieStepBuffer[0] >= 0) {
        for (uint8_t go = 0; go < 22; go++) {
          brewieStep[go] = brewieStepBuffer[go];
          brewieStepBuffer[go] = 0;
        }
        brewieStepBuffer[0] = -1;
        leftTime = brewieStep[STEP_TIME];
        stepTime = 0;
        brewiePause = false;
        newStep = false;
        Process_Step();
      }
      errorOverride = false;
      waterOverride = false;
    } else if (!newStep && !stepActive) {       // Might not be necessary now that I check for this in main loop
      if (brewieStepBuffer[0] == -1) {
        currStep = -1;
      }
    }
  }
}

void Process_Step() {
  // Skip steps if they seem to be of no practical use
  bool zeroTimeStep = false;
  currStep = brewieStep[0];

  float mashSetTemp = (float)brewieStep[STEP_MASH_TEMP]/10.0;
  float boilSetTemp = (float)brewieStep[STEP_BOIL_TEMP]/10.0;

  digitalWrite(INLET_1, LOW);
  digitalWrite(INLET_2, LOW);
  //spargeOverride = false;
  //brewieWaterFill = false;

  // Decide what the step goal is
  switch(brewieStep[STEP_PRIMARY]) {
    case 0: // No known step
      break;
    case 1: // Water inlet
      // Run water fill routine
      brewieWaterFill = true;
      break;
    case 2: // Mash Temp
      break;
    case 3: // Boil Temp
      if (brewieStep[STEP_BOIL_PUMP] == 0) {
        brewieStep[STEP_BOIL_PUMP] = 120;
        brewieStep[STEP_BOIL_INLET] = 1;
        brewieStep[STEP_BOIL_RETURN] = 1;
      }
      break;
    case 4: // Mash Pump Empty
      mashPump->setPumpOut();
      break;
    case 5: // Boil Pump Empty
      boilPump->setPumpOut();
      break;
    case 6: // Run for time
      // If boil step, circulate to prevent scorching
      if (brewieStep[STEP_BOIL_TEMP] == 930) {
        brewieStep[STEP_BOIL_PUMP] = 100;
        brewieStep[STEP_BOIL_INLET] = 1;
        brewieStep[STEP_BOIL_RETURN] = 1;
      }
      if (brewieStep[STEP_TIME] == 0) {
        zeroTimeStep = true;
      }
      break;
    case 7: // Hard Boil
      // Circulate to prevent scorching
      brewieStep[STEP_BOIL_PUMP] = 120;
      brewieStep[STEP_BOIL_INLET] = 1;
      brewieStep[STEP_BOIL_RETURN] = 1;
      brewieStep[STEP_BOIL_TEMP] = 1000;
      boilSetTemp = ((float)brewieStep[STEP_BOIL_TEMP])/10.0;
      break;
    case 8: // Last Step? Prompt? Pause?
      /*if (brewieStep[STEP_TIME] == 0) {
        lastStep = true;
        requestNextStep = true;
      }*/
      break;
    case 9: // Might not exist
      break;
    case 10: // Cool Step/Unclogging step
      if ((float)brewieStep[STEP_BOIL_TEMP]/10.0 < 70.0) {
        brewie->SetCooling();
      } else {
        whirlpoolStep = true;
      }
      // Choose internal or external cooling
      if (externalCooling == true) {
        brewieStep[STEP_COOL_VALVE] = 0;
        brewieStep[STEP_OUTLET] = 1;
        brewieStep[STEP_BOIL_RETURN] = 1;
      } else {
        digitalWrite(INLET_2, HIGH);
      }
      break;
    default: 
      break;
  }
  switch(brewieStep[STEP_SECONDARY]) {
    case 0: // Anything to process?
      break;
    case 1: // Unknown!
      break;
    case 2: // Sparge and Water Calibration (Same as P205?)
    {
      if (brewieStep[STEP_MASH_PUMP] > 0) {
        spargeOverride = true;
        // Override sparge valve and pump settings since they change on their own
        brewieStep[STEP_MASH_PUMP] = 0;
        brewieStep[STEP_BOIL_PUMP] = 0;
        brewieStep[STEP_MASH_INLET] = 0;
        brewieStep[STEP_BOIL_RETURN] = 0;
        brewieStep[STEP_MASH_RETURN] = 0;
        brewieStep[STEP_BOIL_INLET] = 0;
      }
      break;
    }
    case 3: // Hop Additions (the valve dance?)
    {
      uint8_t hopTanks = brewieStep[STEP_HOP_1] + brewieStep[STEP_HOP_2] + brewieStep[STEP_HOP_3] + brewieStep[STEP_HOP_4];
      if (hopTanks == 0) {
        // False alarm, just a delayed hop step
        brewieStep[STEP_BOIL_PUMP] = 120;
        brewieStep[STEP_BOIL_INLET] = 1;
      } else {
        brewieStep[STEP_BOIL_PUMP] = 30 + (50*hopTanks);
      }
      brewieStep[STEP_BOIL_RETURN] = 1;
      brewieStep[STEP_BOIL_TEMP] = 1000;
      boilSetTemp = ((float)brewieStep[STEP_BOIL_TEMP])/10.0;
      if (zeroTimeStep) {
        boilPump->setPumpSpeed(40);
      }
      break;
    }
    case 4: //  Enable water inlet with no water quantity specified
      if (brewieStep[STEP_WATER_INLET] == 1) {
        digitalWrite(INLET_1, HIGH);
      } else {
        digitalWrite(INLET_1, LOW);
      }
      break;
    case 5: // Boil initial fill? (Water for mash tank)
      //mashingWater = brewieStep[STEP_WATER]/10.0;
      break;
    case 6: // Boil sparge fill? (Water for boil tank) Run for time? (from unclogging program)
      //spargeWater = brewieStep[STEP_WATER]/10.0;
      break;
    case 8: // Unclogging last step?
      break;
    default: 
      break;
  }

  // Pumps
  // Mash Pump speed override, to slow it down for mashing
  if (brewieStep[STEP_MASH_PUMP] > 0) {
    if (brewieStep[STEP_MASH_INLET] == 0) {
      brewieStep[STEP_MASH_PUMP] = 255;
    } else if (brewieStep[STEP_MASH_INLET] == 1 && brewieStep[STEP_BOIL_INLET] == 1) {
      brewieStep[STEP_MASH_PUMP] = 255;
    } else if (brewieStep[STEP_PRIMARY] == 2) {
      // Mashing Pump Speed getting to Temp
      brewieStep[STEP_MASH_PUMP] = heatingPumpSpeed;
      brewie->SetMashMaxDuty(1.0);
    } else {
      // Mashing Pump Speed
      brewieStep[STEP_MASH_PUMP] = mashingPumpSpeed;
      brewie->SetMashMaxDuty(0.5);
    }
  }

  // Turn off pumps during valve setup
  if (brewieStep[STEP_MASH_RETURN] == 0) {
    mashPump->setPumpSpeed(0);
  }
  if (brewieStep[STEP_BOIL_RETURN] == 0) {
    boilPump->setPumpSpeed(0);
  } else if (brewieStep[STEP_COOL_VALVE] == 1) {
    boilPump->setPumpSpeed(40);
  }

  // Process valves, but no need to if the step is useless (mainly opening hop valves for no reason)
  if (zeroTimeStep == false) {
    // Valves
    setValve(VALVE_MASH_RET, brewieStep[STEP_MASH_RETURN]);
    setValve(VALVE_BOIL_RET, brewieStep[STEP_BOIL_RETURN]);
    setValve(VALVE_MASH_IN, brewieStep[STEP_MASH_INLET]);
    setValve(VALVE_BOIL_IN, brewieStep[STEP_BOIL_INLET]);
    setValve(VALVE_COOL, brewieStep[STEP_COOL_VALVE]);
    setValve(VALVE_HOP_1, brewieStep[STEP_HOP_1]);
    setValve(VALVE_HOP_2, brewieStep[STEP_HOP_2]);
    setValve(VALVE_HOP_3, brewieStep[STEP_HOP_3]);
    setValve(VALVE_HOP_4, brewieStep[STEP_HOP_4]);
    setValve(VALVE_OUTLET, brewieStep[STEP_OUTLET]); 
  }

  mashPump->setPumpSpeed(brewieStep[STEP_MASH_PUMP]);
  boilPump->setPumpSpeed(brewieStep[STEP_BOIL_PUMP]);

  // Turn on water, for automatic cleaning
  if (brewieStep[STEP_WATER] == 0) {
    if (brewieStep[STEP_WATER_INLET] == 1) {
      digitalWrite(INLET_1, HIGH);
    }
  }
  brewie->setTemperatures(mashSetTemp, boilSetTemp);
  
  stepActive = true;
}

bool Step_Complete() {
  bool stepComplete = false;
  if (currStep >= 0) {
    if (stepActive) {
      switch(brewieStep[STEP_PRIMARY]) {
        case 1: // Water inlet, fill to volume, check for water
          if (waterFilled) {
            digitalWrite(INLET_1, LOW);
            stepActive = false;
            brewieWaterFill = false;
            waterFilled = false;
            boilTankVolume = (float)massSensor/toLiter;
          }
          break;
        case 2: // Run to Mash Temperature
          if (brewie->MashTempReached()) {
            stepActive = false;
          }
          break;
        case 3: // Run to Boil Temperature
          if (brewie->BoilTempReached()) {
            stepActive = false;
          }
          break;
        case 4: // Run until Mash Pump empty
          if (mashPump->pumpIsDry()) {
            stepActive = false;
            setValve(VALVE_MASH_RET, VALVE_CLOSE);
            boilTankVolume += mashTankVolume;
            mashTankVolume = 0.0;
          }
          break;
        case 5: // Run until Boil Pump empty
          if (boilPump->pumpIsDry()) {
            stepActive = false;
            setValve(VALVE_BOIL_RET, VALVE_CLOSE);
            mashTankVolume = boilTankVolume;
            boilTankVolume = 0.0;
          }
          break;
        case 6: // Run until time complete
          if (leftTime == 0) {
            stepActive = false;
            spargeOverride = false;
            // Check for added whirlpool step
            if (whirlpoolStep) {
              // Restore cooling step
              brewieStep[STEP_PRIMARY] = 10;
              brewieStep[STEP_TIME] = whirlpoolTime;
              brewieStep[STEP_BOIL_INLET] = 0;
              brewieStep[STEP_OUTLET] = 0;
              brewieStep[STEP_COOL_VALVE] = 1;
              brewieStep[STEP_BOIL_TEMP] = whirlpoolCoolTemp;
              Process_Step();
              whirlpoolStep = false;
            }
          }
          break;
        case 7: // Something to do with total boil time? Force boil? No boil over?
          if (brewie->BoilTempReached()) {
            stepActive = false;
          }
          break;
        case 8: // User Interaction
          // Requires user interaction (Never finishes, except calibration and unclogging)
          if (brewieStep[STEP_TIME] < 800) {
            if (leftTime == 0) {
              stepActive = false;
            }
          }
          break;
        case 9: // Water level outlet
          break;
        case 10: // Cooling
          if (brewie->BoilTempReached()) {
            if (whirlpoolStep) {
              // Hijack step
              brewieStep[STEP_PRIMARY] = 6;
              brewieStep[STEP_TIME] = whirlpoolTime;
              brewieStep[STEP_BOIL_INLET] = 1;
              brewieStep[STEP_OUTLET] = 0;
              brewieStep[STEP_COOL_VALVE] = 0;
              Process_Step();
            } else {
              stepActive = false;
            }
            brewie->SetHeating();
            digitalWrite(INLET_2, LOW);
          }
          break;
      }
      if (stepActive == false) {
        requestNextStep = true;
        stepComplete = true;
      }
    }
  }
  return stepComplete;
}

void Process_Temperature() {
  //Process 1-Wire Temperatures
  if (powerOn) {
    mashTemp = MashTemp->getTempC(mashAddress);
    mashTemp += mashDelta;
    boilTemp = BoilTemp->getTempC(boilAddress);
    boilTemp += boilDelta;
    if (MashTemp->isConnected(mashAddress)) {
      MashTemp->requestTemperaturesByAddress(mashAddress);
    } else {
      delete MashTemp;
      MashTempPin->reset();
      MashTemp = new DallasTemperature(MashTempPin);
      MashTemp->begin();
      MashTemp->getAddress(mashAddress, 0);
      MashTemp->setCheckForConversion(true);
      MashTemp->setWaitForConversion(false);
    }
    if (BoilTemp->isConnected(boilAddress)) {
      BoilTemp->requestTemperaturesByAddress(boilAddress);
    } else {
      delete BoilTemp;
      BoilTempPin->reset();
      BoilTemp = new DallasTemperature(BoilTempPin);
      BoilTemp->begin();
      BoilTemp->getAddress(boilAddress, 0);
      BoilTemp->setCheckForConversion(true);
      BoilTemp->setWaitForConversion(false);
    }
    if (mashTemp == -127.0) {
      sprintf(errorMessage[errorCount], "E%d", 115);
      errorCount++;
    }
    if (boilTemp == -127.0) {
      sprintf(errorMessage[errorCount], "E%d", 116);
      errorCount++;
    }
  }
}

void Process_Sensors() {
  static uint32_t sensorTime = millis();
  static uint32_t sensorArray[4] = { 0, 0, 0, 0 };
  static uint16_t samples = 0;
  static uint32_t massAve = 0;
  static uint8_t massSamp = 0;
  static uint32_t massTime = millis();

  float timeDiff = millis() - sensorTime;
  if (timeDiff > 500) {
    sensorTime = millis();
    //Serial.println(samples);

    if (samples > 100) {
      acCurrent = (float)(sensorArray[0]/(float)samples)/12.0;
      acMeasure += acCurrent/3600.0*(float)machineVoltage*((float)timeDiff/1000.0);             // Convert back to per sample period basis
      boardTemp = (uint16_t)((((float)analogRead(BOARD_TEMP)*5.0/1023.0)/6.1-0.07)*1000.0);
      inlet1A   = (float)(sensorArray[1]/(float)samples);
      boilPumpA = (float)(sensorArray[2]/(float)samples);
      mashPumpA = (float)(sensorArray[3]/(float)samples);
  
      for (uint8_t clr = 0; clr < 4; clr++) {
        sensorArray[clr] = 0;
      }
      
      samples = 0;
    }
  }
  
  // Process Analog Inputs
  uint16_t acTemp = analogRead(AC_MEAS);
  if (acTemp > 7) {
    acTemp -= 8;
    float peakTemp = (float)acTemp/12.0*0.7071;
    if (peakTemp > acPeakCurrent) {
      acPeakCurrent = peakTemp;
    }
    // On a cycle-by-cycle basis, check for high current
    if (acPeakCurrent > 16.0) {
      // Do what????
    }
    
  } else {
    acTemp = 0;
  }
  uint16_t cycleCurrent = 0;
  sensorArray[0] += acTemp;
  uint16_t tempCurrent = analogRead(I_INLET1);
  cycleCurrent += tempCurrent;
  tempCurrent = analogRead(I_INLET2);
  cycleCurrent += tempCurrent;
  sensorArray[1] += tempCurrent;
  tempCurrent = analogRead(I_BOIL_PUMP);
  cycleCurrent += tempCurrent;
  sensorArray[2] += tempCurrent;
  if (tempCurrent > 950) {
    Serial.println("!High Current on Boil Pump");
    boilPump->setPumpSpeed(0);
  }
  tempCurrent = analogRead(I_MASH_PUMP);
  if (tempCurrent > 950) {
    Serial.println("!High Current on Mash Pump");
    mashPump->setPumpSpeed(0);
  }
  cycleCurrent += tempCurrent;
  sensorArray[3] += tempCurrent;
  samples++;

  if (cycleCurrent > 1800) {
    Serial.println("!Power brownout imminent!");
  }

  // Pressure sensor seems to take > 100ms to take a reading
  // Process I2C Weight/Pressure Sensors
  if (powerOn) {
    if (millis() - massTime > 44) {
      massTime = millis();

      if (PressureReadAll(&massFast, &massTemp) == 1) {
        massSensor = 0xFFFF;
        TWIInit();
        if (errorCount == 0) {
          sprintf(errorMessage[errorCount], "E%d", 118);
          errorCount++;
        }
      }
      massTemp = massTemp >> 5;

      uint8_t massPlusStatus = (massFast >> 14);
      uint8_t massStatus = (massPlusStatus & 0x03);
      
      // Check status flags for new data or error
      if (massStatus == 0) {
        validMass = true;
        massFast = massFast & 0x3FFF;
        massTempInC = (float)(massTemp - 512)/10.0;
        
        // Mass temp sensor is slow to react, but tank temperature is too fast. Split difference?
        float tempAdjust = boilTemp;//(massTempInC + boilTemp)/2.0;
        if (boilTemp > (massTempInC + 5.0)) {
          tempAdjust = massTempInC;
        }
        massOffset = (int16_t)(((tempAdjust - pressureTempCal)*toLiter)*0.15);
        if (massFast > massOffset) {
          massFast -= massOffset;
        } else {
          massFast = 0;
        }
        // Don't add to main water level accumulator if reading isn't going to be accurate
        if (boilPump->pumpTach() < 5 && !boilPump->isRunning()) {
          massAve += massFast;
          massSamp++;
        }
      }
  
      if (massSamp > 7) {
        massSensor = (uint16_t)((float)massAve/(float)massSamp);
        if (fastWaterReadings) {
          int16_t adjMass = (int16_t)massSensor - (int16_t)toLiterNull;
          if (adjMass < 0) {
            adjMass = 0;
          }
          waterVolume = (float)adjMass/toLiter;
        } else {
          if (massSensor > ((uint16_t)toLiterNull)) {
            massSensor -= (uint16_t)toLiterNull;
          } else {
            massSensor = 0;
          }
          waterVolume = (float)massSensor/toLiter;
        }
        massAve = 0;
        massSamp = 0;
      }
    }
  }
}

void Close_All_Valves() {
  brewie->Reset();
  digitalWrite(INLET_1, LOW);
  digitalWrite(INLET_2, LOW);
  mashPump->setPumpSpeed(0);
  boilPump->setPumpSpeed(0);
  setValve(VALVE_OUTLET, VALVE_CLOSE);
  setValve(VALVE_MASH_RET, VALVE_CLOSE);
  setValve(VALVE_BOIL_RET, VALVE_CLOSE);
  setValve(VALVE_BOIL_IN, VALVE_CLOSE);
  setValve(VALVE_MASH_IN, VALVE_CLOSE);
  setValve(VALVE_HOP_1, VALVE_CLOSE);
  setValve(VALVE_HOP_2, VALVE_CLOSE);
  setValve(VALVE_HOP_3, VALVE_CLOSE);
  setValve(VALVE_HOP_4, VALVE_CLOSE);
  setValve(VALVE_COOL, VALVE_CLOSE);
}

void Run_Sparge() {
  static uint16_t spargeNull = 0;
  static uint16_t boilHigh  = 0;
  static uint16_t boilLow = 0;
  static uint16_t mashSide = 2000;
  static uint8_t spargeStep = 0;
  static uint32_t spargeTime = 0;
  static uint16_t flowResult = 0;
  static uint16_t mashFlowResult = 0;
  if (spargeOverride && !brewiePause) {
    switch (spargeStep) {
      case 0:
        mashSide = 2000;
        mashPump->pumpFlowReset();
        boilPump->pumpFlowReset();
        spargeTime = millis();
        spargeStep++;
        break;
      case 1:
        if (millis() - spargeTime > 4000) {
          spargeNull = massSensor;
          boilLow = spargeNull;
          // Worth it???
          if (spargeNull < (uint16_t)(22*toLiter)) {
            spargeStep++;
          } else {
            // Too much water error
            sprintf(&brewieMessage[messageCount][0], "E%d%d", 0, 12);
            messageCount++;
            Brewie_Pause();
          }
        }
        break;
      case 2:
        if (spargeNull < (uint16_t)(22*toLiter)) {
          // Run mash pump to boil side
          setValve(VALVE_BOIL_RET, VALVE_CLOSE);
          setValve(VALVE_MASH_IN, VALVE_CLOSE);
          setValve(VALVE_MASH_RET, VALVE_OPEN);
          setValve(VALVE_BOIL_IN, VALVE_OPEN); 
          mashPump->setPumpSpeed(spargePumpSpeed);
          spargeTime = millis();
          spargeStep++;
        } else {
          // Too much water error
          sprintf(&brewieMessage[messageCount][0], "E%d%d", 0, 12);
          messageCount++;
          Brewie_Pause();
        }
        break;
      case 3:
        if (mashPump->pumpFlow() > mashSide) {
          spargeStep++;
          mashPump->setPumpSpeed(0);
          setValve(VALVE_MASH_RET, VALVE_CLOSE);
          setValve(VALVE_BOIL_IN, VALVE_CLOSE);
          spargeTime = millis();
        } else if (millis() - spargeTime > 60000) {
          spargeStep++;
        }
        break;
      case 4:
        // Run boil pump to mash side
        if (millis() - spargeTime > 4000) {
          boilHigh = massSensor;
          setValve(VALVE_BOIL_RET, VALVE_OPEN);
          setValve(VALVE_MASH_IN, VALVE_OPEN);
          boilPump->setPumpSpeed(spargePumpSpeed);
          spargeTime = millis();
          spargeStep++;
        }
        break;
      case 5:
        // Stop when pump ticks are equal
        if (boilPump->pumpFlow() > 2000) {
          spargeStep++;
        } else if (millis() - spargeTime > 60000) {
          spargeStep++;
        }
        break;
      case 6:
        // Stop
        boilPump->setPumpSpeed(0);
        setValve(VALVE_BOIL_RET, VALVE_CLOSE);
        setValve(VALVE_MASH_RET, VALVE_OPEN);
        setValve(VALVE_BOIL_IN, VALVE_CLOSE);
        setValve(VALVE_MASH_IN, VALVE_OPEN);
        mashFlowResult = mashPump->pumpFlow();
        mashPump->setPumpSpeed(spargePumpSpeed);
        spargeTime = millis();
        spargeStep++;
        break;
      case 7:
        // Wait for things to settle
        if (millis() - spargeTime > 15000) {
          spargeStep++;
        }
        break;
      case 8:
      {
        // Adjust sparge routine to even out water level
        int16_t tachDiff = (int16_t)(boilPump->pumpFlow() - (mashSide - mashFlowResult));
        int16_t deltaBoil = (int16_t)(boilHigh - boilLow);
        boilLow = massSensor;
        int16_t deltaMash = (int16_t)(boilHigh - boilLow);
        mashSide = mashSide/2 + 1000 + (deltaMash - deltaBoil) + (int16_t)(spargeNull - massSensor)*2 + (tachDiff/5);
        if (mashSide < 800) {
          mashSide = 800;
        } else if (mashSide > 3200) {
          mashSide = 3200;
        }
        Serial.print("Sparge Null: ");
        Serial.print(spargeNull);
        Serial.print("Mash Adjustment: ");
        Serial.print(mashSide);
        Serial.print(" Mash Side: ");
        Serial.println(deltaMash);
        Serial.print(" Boil Side: ");
        Serial.println(deltaBoil);
        spargeStep = 2;
        mashPump->pumpFlowReset();
        boilPump->pumpFlowReset();
        break;
      }
      default:
        spargeStep = 0;
        break;
    } 
  } else {
    spargeStep = 0;
  }
}

// Automatic water fill:
// TO-DO:
//  There should be clear filling goals, like 50% initially, and clear over and under bounds.
//  Converting units is a pain, and every cycle it should convert units to raw units to make things simpler.
//  Maybe like stock firmware, the pump should run to verify that it is empty. But, if we can get a baseline
//    calibration for temperature, we can know that ~1-2L is just a temperature artifact.
//  Pump running for 2 minutes to cool it down could probably be improved.
void Fill_Boil_Tank() {
  static uint8_t fillStep = 0;
  static uint8_t fillAttempts = 0;
  static float initialMass = 0;
  static uint16_t initialTemp = 0;
  static float fillTarget = 0;
  static uint32_t startTime = 0;
  static uint16_t massTempPrevious = 0;
  static uint32_t temperatureTime = 0;
  static uint32_t waterTimeout = 0;
  
  if (brewieWaterFill && !brewiePause) {
    //uint16_t fillTargetRaw = (fillTarget*toLiter)+toLiterNull;
    //int16_t offsetMass = massFast - (initialMass*toLiter + toLiterNull);
    switch (fillStep) {
      case 0:
        // Initialize variables
        fillAttempts = 0;
        initialMass = 0;
        initialTemp = 0;
        waterTime = millis();
        startTime = millis();
        waterTimeout = (uint32_t)brewieStep[STEP_WATER]*2000;
        fillStep++;
        break;
      case 1:
        // Wait to let water settle, just in case
        if (millis() - waterTime > 5000) {
          initialMass = (float)massSensor/toLiter;
          initialTemp = massTemp;
          fillTarget = initialMass + 4.0;
          if (fillTarget >= ((float)brewieStep[STEP_WATER]/10.0)) {
            fillTarget = ((float)brewieStep[STEP_WATER]/10.0)-1.0;
            if (fillTarget < initialMass) {
              fillTarget = ((float)brewieStep[STEP_WATER]/10.0);
            }
          }
          Serial.print("Target Volume: ");
          Serial.println(fillTarget);
          fillStep++;
        }
        break;
      case 2:
        // Decide which path to follow based on initial conditions
        Serial.print("Initial Mass: ");
        Serial.println(initialMass);
        Serial.print("Initial Temp: ");
        Serial.println(initialTemp);
        // If the pressure sensor temperature is high, fill relative to the current volume
        if (initialTemp > 800) {
          waterTimeout = 20000;
          digitalWrite(INLET_1, HIGH);
          fillStep++;
          Serial.println("Temperature high, cooling down by circulating water");
        } else {
          if (initialMass < (fillTarget*0.96)) {
            digitalWrite(INLET_1, HIGH);
            fillStep++;
          } else if ((initialMass-0.5) > ((float)brewieStep[STEP_WATER]/10.0)) {
            fillStep = 13;
          } else {
            fillStep = 6;
          }
        }
        waterTime = millis();
        break;
      // Fill to volume
      case 3:
        if (initialTemp > 800) {
          if ((millis() - waterTime) > waterTimeout) {
            fillStep = 4;
            Serial.println("Water Timeout hit");
          }
        } else {
          if ((float)((massFast-toLiterNull)/toLiter) < fillTarget*0.95) {
            // Water level still low
          } else {
            // Water at level, shut down
            fillStep = 4;
          }
          if (millis() - waterTime > waterTimeout) {
            if ((float)(massSensor/toLiter) < fillTarget*0.65) {
              fillStep = 12;
              digitalWrite(INLET_1, LOW);
            } else {
              fillStep = 4;
            }
          }
        }
        break;
      case 4:
        // Shut down
        digitalWrite(INLET_1, LOW);
        fillStep++;
        waterTime = millis();
        break;
      case 5:
        // Wait 5 seconds, check water level, and adjust as needed
        if (millis() - waterTime > 5000) {
          // Restart if water level is still very low
          if ((float)(massSensor/toLiter) < fillTarget*0.65) {
            fillAttempts++;
            fillStep = 2;
          } else {
            fillStep = 6;
          }
          if (fillAttempts > 3) {
            fillStep = 12;
          }
          float flowRate = ((float)(massSensor/toLiter) - initialMass)/((float)(millis() - startTime))*60000.0;
          Serial.print("Filled: ");
          Serial.println((float)(massSensor/toLiter) - initialMass);
          Serial.print("Fill Rate: ");
          Serial.print(flowRate);
          Serial.println("L/min");
        }
        break;
      case 6:
        setValve(VALVE_BOIL_RET, VALVE_OPEN);
        setValve(VALVE_BOIL_IN, VALVE_PINCH_ANGLE);
        boilPump->setPumpSpeed(255);
        waterTime = millis();
        temperatureTime = waterTime;
        massTempPrevious = massTemp;
        fillStep++;
        break;
      case 7:
        // Heat affects the accuracy, so run pump to acclimate
        //if (((millis() - waterTime) > 45000) && (massTemp < (waterInletTemp + 30)) || (millis() - waterTime) > 180000) {
        if (((millis() - waterTime) > 45000) && (massTemp < (waterInletTemp + 10)) || (millis() - waterTime) > 180000) {
          if(boilPump->pumpIsDry()) {
            fillStep = 14;
          } else {
            fillStep++;
          }
          boilPump->setPumpSpeed(0);
          setValve(VALVE_BOIL_RET, VALVE_CLOSE);
          setValve(VALVE_BOIL_IN, VALVE_CLOSE);
          waterTime = millis();
          fillTarget = ((float)brewieStep[STEP_WATER]/10.0);
        }
        if (millis() - temperatureTime > 20000) {
          temperatureTime = millis();
          if (massTemp > (massTempPrevious - 4)) {
            // Reached steady state
            waterInletTemp = massTemp;
          }
          massTempPrevious = massTemp;
        }
        if(boilPump->pumpIsDry()) {
          fillStep = 2;
        }
        break;
      case 8:
        // Passed all steps so far...
        if (millis() - waterTime > 8000) {
          waterTimeout = (uint32_t)brewieStep[STEP_WATER]*2000;
          //initialMass = (float)massSensor/toLiter;
          if (((float)massSensor/toLiter) < (fillTarget - 0.1)) {
            digitalWrite(INLET_1, HIGH);
            delay(250);
            fillStep++;
          } else if (((float)massSensor/toLiter - 0.5) > fillTarget) {
            fillStep = 13;
          } else {
            fillStep = 11;
          }
          waterTime = millis();
        }
        break;
      case 9:
        if ((float)((massFast-toLiterNull)/toLiter) < (fillTarget - 0.05)) {
          // Water level still low
        } else {
          // Water at level, shut down
          fillStep++;
        }
        if (millis() - waterTime > (waterTimeout/4)) {
          fillStep++;
        }
        break;
      case 10:  
        // Shut down
        digitalWrite(INLET_1, LOW);
        fillStep++;
        waterTime = millis();
        break;
      case 11:
        // Wait 5 seconds, check water level, and adjust as needed
        if (millis() - waterTime > 5000) {
          if (fillAttempts > 6) {
            fillStep = 12;
          } else if (((float)massSensor/toLiter) < (fillTarget - 0.1)) {
            fillStep = 8;
            fillAttempts++;
          } else {
            // Filled to volume
            fillStep = 0;
            if (massTemp < (massTempPrevious - 20)) {
              Serial.println("Temperature still changing");
              //fillStep = 6;
            } else {
              waterFilled = true;
              brewieWaterFill = false;
              waterInletTemp = massTemp;
            }
          }
          waterTime = millis();
        }
        break;
      case 12:
        // No Inlet
        brewieWaterFill = false;
        Brewie_Pause();
        sprintf(&errorMessage[errorCount][0], "E005");
        errorCount++;
        fillStep = 0;
        break;
      case 13: 
        // Too much water
        if (waterOverride) {
          waterFilled = true;
          brewieWaterFill = false;
          fillStep = 0;
        } else {
          safetyShutdown = true;
          brewieWaterFill = false;
          Brewie_Pause();
          sprintf(&errorMessage[errorCount][0], "E012");
          errorCount++;
          fillStep = 0;
        }
        break;
      case 14:
        // No Water Detected
        brewieWaterFill = false;
        Brewie_Pause();
        sprintf(&errorMessage[errorCount][0], "E011");
        errorCount++;
        fillStep = 0;
        break;
      default:
        fillStep = 0;
        break;
    }
  } else {
    fillStep = 0;
    //fastWaterReadings = false;
  }
}

void Safety_Check() {
  static uint8_t safetyCheckCount = 0;
  static uint8_t currentStep = -1;
  static uint8_t heaterCheck = 0;
  float currentLimit = 0.0;

  if (currentStep != brewieStep[0]) {
    currentStep = brewieStep[0];
    safetyCheckCount = 0;
  }
  if (stepActive == false) {
    safetyCheckCount = 0;
  }

  if (machineVoltage == 240) {
    currentLimit = 12.0;
  } else {
    currentLimit = 20.0;
  }
  
  // Overcurrent check
  if (acCurrent > currentLimit) {
    // Shut down heaters immediately
    safetyShutdown = true;
  } else if (acCurrent < 1.0) {
    // Heater undercurrent check (should be ~10-12 for 120V, 7-10 for 240V)
    if (brewie->MashHeaterOn()) {
      if (digitalRead(POWER_LIGHT) == LOW) {
        digitalWrite(POWER_LIGHT, HIGH);
      } else {
        digitalWrite(POWER_LIGHT, LOW);
      }
    } else if (brewie->BoilHeaterOn()) {
      if (digitalRead(DRAIN_LIGHT) == LOW) {
        digitalWrite(DRAIN_LIGHT, HIGH);
      } else {
        digitalWrite(DRAIN_LIGHT, LOW);
      }
    } else {
      digitalWrite(POWER_LIGHT, LOW);
      digitalWrite(DRAIN_LIGHT, LOW);
    }
  } else {
    if (brewie->MashHeaterOn()) {
      digitalWrite(POWER_LIGHT, HIGH);
    } else {
      digitalWrite(POWER_LIGHT, LOW);
    }
    if (brewie->BoilHeaterOn()) {
      digitalWrite(DRAIN_LIGHT, HIGH);
      if (machineVoltage == 0) {
        if (acCurrent > 10) {
          machineVoltage = 120;
        } else {
          machineVoltage = 240;
        }
      }
    } else {
      digitalWrite(DRAIN_LIGHT, LOW);
    }
  }
  // Turn off boil heater if no water detected
  if (!errorOverride) {
    // Empty tank safety
    if (brewie->BoilHeaterOn()) {
      // Pressure sensor is too unreliable when pump is running or right after it's turned off
      if (boilPump->pumpTach() == 0) {
        if ((float)((massSensor)/toLiter) < 2.0) {
          safetyCheckCount++;
        }
      } else if (boilPump->pumpIsDry()) {
        safetyCheckCount++;
      }
    }
    if (brewie->MashHeaterOn()) {
      if (mashPump->pumpIsDry()) {
        safetyCheckCount++;
      }
    }
  }

  // Internal temperature safety
  if (boardTemp > 350) {
    digitalWrite(VENT_FAN, HIGH);
    digitalWrite(POWER_FAN, HIGH);
    if (boardTemp > 400) {
      //safetyCheckCount++;
    }
  } else if (!brewing && !fanOverride && boardTemp < 320) {
    digitalWrite(VENT_FAN, LOW);
    digitalWrite(POWER_FAN, LOW);
  }

  // If heaters seem off for more than a second or so, increment counter
  if (brewie->ReadMashError()) {
    safetyCheckCount+=6;
    if (safetyCheckCount > 10) {
      safetyCheckCount = 0;
      safetyShutdown = true;
      sprintf(&errorMessage[errorCount][0], "E%d%d%d", 0, 0, 7);
      errorCount++;
    }
  }
  if (brewie->ReadBoilError()) {
    safetyCheckCount+=6;
    if (safetyCheckCount > 10) {
      safetyCheckCount = 0;
      safetyShutdown = true;
      sprintf(&errorMessage[errorCount][0], "E%d%d%d", 0, 0, 8);
      errorCount++;
    }
  }
  
  if (safetyCheckCount > 1) {
    Serial.print("Safety Check Count: ");
    Serial.println(safetyCheckCount);
  }

  if (mashPump->pumpIsClogged()) {
    if (errorCount == 0) {
      sprintf(&errorMessage[errorCount][0], "E%d%d%d", 0, 0, 9);
      errorCount++;
    }
  }
  if (boilPump->pumpIsClogged()) {
    if (errorCount == 0) {
      sprintf(&errorMessage[errorCount][0], "E%d%d%d", 0, 1, 0);
      errorCount++;
    }
  }
  
  // If error message happens at same time as next step, no message will appear on GUI at all!
  if (safetyShutdown && !brewiePause && !errorOverride) {
    Brewie_Pause();
    if (errorCount == 0) {
      sprintf(&errorMessage[errorCount][0], "E%d%d%d", 0, 2, 0);
      errorCount++;
    }
  }
}

void Brewie_Status() {
  // Print out status info every second
  uint32_t currTime = millis();
  uint32_t timeDiff = currTime - statusTime;
  if (timeDiff >= 999) {
    statusTime = currTime;
    uint8_t extraTime = (timeDiff/999);
    leftOverTime += timeDiff % 999;
    if (leftOverTime > 1000) {
      extraTime++;
      leftOverTime = leftOverTime % 1000;
    }

    // Increment/Decrement times
    if (currStep > -1) {
      if (!brewiePause) {
        totalTime += extraTime;
        stepTime += extraTime;
        if (leftTime >= extraTime) {
          leftTime -= extraTime;
        }

        // Safety control for extremely long times
        if (totalTime > 60000) {
          totalTime = 59999;
          Brewie_Pause();
        }
        if (stepTime > 30000) {
          stepTime = 29999;
          Brewie_Pause();
        }
      } else {
        currStep = -2;
      }
    }
    
    if (powerButton) {
      sprintf(brewieMessage[messageCount], "P%d", 999);
      messageCount++;
      powerButton = false;
    }
    if (drainButton) {
      sprintf(brewieMessage[messageCount], "P%d", 998);
      messageCount++;
      drainButton = false;
    }

    Process_Temperature();
    brewie->Temperature_Average();
    
    mashPump->Pump_Speed_Control((uint16_t)mashPumpA);
    boilPump->Pump_Speed_Control((uint16_t)boilPumpA);

    // Pump out special workflow
    if (brewieStep[STEP_PRIMARY] == 4 && stepActive) {
      if (mashPump->requestPinchValve()) {
        if (brewieStep[STEP_BOIL_INLET] == 1) {
          setValve(VALVE_BOIL_IN, VALVE_PINCH_ANGLE);
          setValve(VALVE_BOIL_RET, VALVE_CLOSE);
        }
      } else if (mashPump->pumpIsDry()) {
        setValve(VALVE_MASH_RET, VALVE_CLOSE);
      }
    }
    if (brewieStep[STEP_PRIMARY] == 5 && stepActive) {
      if (boilPump->requestPinchValve()) {
        if (brewieStep[STEP_MASH_INLET] == 1) {
          setValve(VALVE_MASH_IN, VALVE_PINCH_ANGLE);
          //setValve(VALVE_MASH_RET, VALVE_CLOSE);
        }
      } else if (boilPump->pumpIsDry()) {
        setValve(VALVE_BOIL_RET, VALVE_CLOSE);
      }
    }

    // Water Inlet functionality - Prevent Overflows
    if (brewieStep[STEP_SECONDARY] == 4) {
      if (massSensor/toLiter > 10.0) {
        digitalWrite(INLET_1, LOW);
      } else if (massSensor/toLiter < 5.0) {
        if (brewieStep[STEP_WATER_INLET] == 1) {
          digitalWrite(INLET_1, HIGH);
        }
      }
    }

    // Step Change State Machine
    // Brewie seems to like a delay between messaging and reporting, so requesting a new
    //  step should happen before incrementing steps, and pauses and errors/warnings 
    //  should also happen in the current step, before reporting a pause or reset condition.
    //  The current step variable gets updated in the step change functions, but the pause
    //  condition needs to get handled here.
    //
    //  This state machine essentially prevents new steps from being requested until the first
    //  request is either successful, or times out. This makes it so that when steps are finished
    //  immediately (like when the temperature is already above the set point) we don't get into a
    //  condition where a single request was sent when two steps were completed.
    switch(stepChangeState) {
      case 0:
        if (requestNextStep) {
          requestNextStep = false;
          if (lastStep) {
            currStep = -1;
            lastStep = false;
          } else {
            brewieStepRequestFlag = true;
            sprintf(&brewieMessage[messageCount][0], "P%d", 103);
            messageCount++;
            // Add remaining time to keep total time "accurate"
            totalTime += leftTime;
            stepTime = 0;
            leftTime = 0;

            stepChangeState++;
            stepChangeTime = millis();
          }
        } else if (brewiePause && currStep > -1) {
          currStep = -2;
        } else {
          // If we are way into a step without a new one in the buffer, try getting the step again
          // (kind of a catch all, hopefully)
          if (!lastStep && brewieStepBuffer[0] == -1 && stepActive && stepTime > 10 && currStep > 0) {
            Serial.println("Missing next step, trying again...");
            requestNextStep = true;
          }
        }
        if (brewieStepRequestFlag == false) {
          // A new step was sent, out of turn?
          if (lastStep == true) {
            lastStep = false;
          }
        }
        break;
      // Request Step Change
      case 1:
        if (brewieStepRequestFlag == false) {
          // Successfully got next step
          stepChangeState = 0;
        } else if (millis() - stepChangeTime > 2000) {
          // 2 second timeout. Maybe out of steps
          stepChangeState = 0;
          lastStep = true;
        } else {
          /*if (brewieStep[STEP_PRIMARY] == 8) {
            currStep = -1;
          }*/
        }
        break;
    }

    Safety_Check();
    if (powerOn) {
      if (!calibrationSent) {
        digitalWrite(POWER_LIGHT, HIGH);
      }
    }
    Step_Change();
    
    Brewie_Status_Write();
   
    // Reset status strings and counts
    brewieMessage[0][0] = 0;
    errorMessage[0][0] = 0;
    echoCount = 0;
    newCommand = false;
  
    messageCount = 0;
    errorCount = 0;
  }
}

void Initialize_2560() {
  PRR1 = (1<<PRUSART1) | (1<<PRUSART2) | (1<<PRUSART3);
  DIDR2 = 0x7F; // ADC8-14 are analog ins
  PORTK |= 0x80;  // ADC15 input high

  // Set unused pins to input high
  DDRE &= ~0xC8;
  PORTE |= 0xC8;
  DDRH &= ~0x3F;
  PORTH |= 0x3F;
  DDRB &= ~0x09;
  PORTB |= 0x09;
  DDRG &= ~0x1C;
  PORTG |= 0x1C;
  DDRL &= ~0xDF;
  PORTL |= 0xDF;
  DDRD &= ~0x48;
  PORTD |= 0x48;
  DDRC &= ~0x55;
  PORTC |= 0x55;
  DDRJ &= ~0x83;
  PORTJ |= 0x83;
  DDRF &= ~0xF8;
  PORTF |= 0xF8;
  
  // Set up Timer 4 for Servos
  TCCR4A = 0;//_BV(WGM41);
  TCCR4B = _BV(WGM43) |  _BV(CS41); 
  ICR4 = 20000;
  OCR4A = 1000;
  //TIMSK4 |= _BV(TOIE4) + _BV(OCIE4A);
  // Set up Valve pins + Inlets
  DDRJ |= 0x7C;
  DDRA |= 0xFF;
  DDRC |= 0x02;

  // Set up Timer 5 for lights
  //TCCR5A = _BV(COM5C0);
  TCCR5B = _BV(WGM51) | _BV(CS50);
  ICR5 = 250;
  OCR5C = 1;
  TIMSK5 &= ~(_BV(TOIE5) + _BV(OCIE5C));

  pinMode(VENT_FAN, OUTPUT);
  pinMode(POWER_FAN, OUTPUT);
  pinMode(LED2, OUTPUT);
  pinMode(LED1, OUTPUT);
  pinMode(INLET_1, OUTPUT);
  pinMode(INLET_2, OUTPUT);

  // SPI for DAC
  pinMode(SPEED_CTRL_SCLK, OUTPUT);
  pinMode(SPEED_CTRL_MOSI, OUTPUT);
  pinMode(SPEED_CTRL_LDAC, OUTPUT);
  pinMode(SPEED_CTRL_CS, OUTPUT);
  digitalWrite(SPEED_CTRL_LDAC, LOW);  // ~LDAC (Leave low to have output latch at end of SPI transfer)
  digitalWrite(SPEED_CTRL_CS, HIGH); // DAC ~CS
  pinMode(50, INPUT);   //MISO, DAC doesn't send any data though
  pinMode(53, OUTPUT);  //Main ~CS, needs to be output for SPI to work

  pinMode(PWR_EN_SERVO, OUTPUT);
  digitalWrite(PWR_EN_SERVO, LOW);
  
  // Power Enable 12V
  DDRH |= 0x80;
  pinMode(PWR_EN_5V, OUTPUT);
  pinMode(PWR_EN_ARM, OUTPUT);
  
  digitalWrite(PWR_EN_5V, LOW);
  pinMode(MASH_PUMP_TACH, INPUT_PULLUP);
  pinMode(BOIL_PUMP_TACH, INPUT_PULLUP);
  SPI.beginTransaction(SPISettings(100000, MSBFIRST, SPI_MODE0));
  SPI.endTransaction();
  //SPI.begin();
  
  pinMode(POWER_BUTTON, INPUT);
  pinMode(DRAIN_BUTTON, INPUT);
  pinMode(POWER_LIGHT, OUTPUT);
  pinMode(DRAIN_LIGHT, OUTPUT);
  digitalWrite(POWER_LIGHT, LOW);
  digitalWrite(DRAIN_LIGHT, LOW); 
  
  Serial.begin(115200);

  TWIInit();

  attachInterrupt(digitalPinToInterrupt(BOIL_PUMP_TACH), boilTachISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(MASH_PUMP_TACH), mashTachISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(DRAIN_BUTTON), drainButtonISR, RISING);

  MashTempPin = new OneWire(MASH_TEMP);
  BoilTempPin = new OneWire(BOIL_TEMP);

  statusTime = millis();

  mashPump = new Pump(MASH_PUMP, 1);
  boilPump = new Pump(BOIL_PUMP, 0);
  brewie = new Brewie;
  /*Pump mashPump(MASH_PUMP, 1);
  Pump boilPump(BOIL_PUMP, 0);
  Brewie Brewie;*/
  
  brewie->setTemperatureSensors(&mashTemp, &boilTemp);
  brewie->setPowerSensor(&acMeasure);

  mashPump->pumpTicks = &mashTicks;
  boilPump->pumpTicks = &boilTicks;

  Brewie_Reset();
}

void Brewie_Reset() {
  currStep = -1;
  brewieStep[0] = -1;
  brewieStepBuffer[0] = -1;
  totalTime = 0;
  stepTime = 0;
  leftTime = 0;
  lastStep = false;
  brewing = false;
  brewieWaterFill = false;
  spargeOverride = false;
  safetyShutdown = false;
  errorOverride = false;
  //fanOverride = false;
  stepActive = false;
  requestNextStep = false;
  brewieStepRequestFlag = false;
  stepChangeState = 0;
  brewie->Reset();
  mashPump->setPumpSpeed(0);
  boilPump->setPumpSpeed(0);
  digitalWrite(INLET_1, LOW);
  digitalWrite(INLET_2, LOW);
}

void Power_On() {
  digitalWrite(POWER_LIGHT, HIGH);
  //PORTH |= 0x80;
  //delay(1);
  //digitalWrite(PWR_EN_5V, LOW);
  for (uint8_t x = 0; x<25;x++) {
    //digitalWrite(PWR_EN_ARM, HIGH);
    PORTB |= 0x40;
    delayMicroseconds(1);
    //digitalWrite(PWR_EN_ARM, LOW);
    PORTB &= ~0x40;
    delayMicroseconds(20);
  }
  digitalWrite(PWR_EN_ARM, HIGH);
  delay(400);
  PORTH |= 0x80;
  delay(100);
  //digitalWrite(PWR_EN_5V, HIGH);
  //Close_All_Valves();
  powerOn = true;
  TIMSK5 |= _BV(TOIE5) + _BV(OCIE5C);
}

void Power_Off() {
  Brewie_Reset();
  setValve(VALVE_OUTLET, VALVE_CLOSE);
  setValve(VALVE_MASH_IN, VALVE_OPEN);
  setValve(VALVE_BOIL_IN, VALVE_OPEN);
  setValve(VALVE_HOP_1, VALVE_OPEN);
  setValve(VALVE_HOP_2, VALVE_OPEN);
  setValve(VALVE_HOP_3, VALVE_OPEN);
  setValve(VALVE_HOP_4, VALVE_OPEN);
  setValve(VALVE_COOL, VALVE_OPEN); 
  setValve(VALVE_MASH_RET, VALVE_OPEN);
  setValve(VALVE_BOIL_RET, VALVE_OPEN);
  digitalWrite(VENT_FAN, LOW);
  digitalWrite(POWER_FAN, LOW);
  digitalWrite(PWR_EN_SERVO, LOW);
  PORTH &= ~0x80;
  digitalWrite(PWR_EN_ARM, LOW);
  digitalWrite(PWR_EN_5V, LOW);
  digitalWrite(POWER_LIGHT, LOW);
  calibrationSent = false;
  powerOn = false;
  TIMSK5 &= ~(_BV(TOIE5) + _BV(OCIE5C));
  digitalWrite(LED1, LOW);
  digitalWrite(LED2, LOW);
}

void Brewie_Pause() {
  brewiePause = true;
  if (currStep >= 0) {
    //brewiePause = true;
    brewieStep[STEP_TIME] = leftTime;
    //currStep = -2;
    Close_All_Valves();
    //brewieWaterFill = false;
    //spargeOverride = false;
    brewing = false;
  }
}

void Fast_Water_Readings() {
  if (fastWaterReadings && brewing) {
    if (validMass) {
      Serial.print("RL");
      Serial.println(massFast);
      validMass = false;
    }
  }
}

ISR(TIMER4_COMPA_vect){
  cli();
  *Valve_Port[valvePWM] &= ~Valve_Bitmask[valvePWM];
  /*if (OCR4B > OCR4A) {
    OCR4A += 100;
  } else {
    OCR4A -= 100;
  }*/
  sei();
}

ISR(TIMER4_OVF_vect){
  cli();
  *Valve_Port[valvePWM] |= Valve_Bitmask[valvePWM];
  sei();
}

ISR(TIMER5_COMPC_vect){
  cli();
  //PORTL &= ~0x20;
  PORTF &= ~0x02;
  sei();
}

ISR(TIMER5_OVF_vect){
  cli();
  //PORTL |= 0x20;
  PORTF |= 0x02;
  if (OCR5A == 1) {
    OCR5C--;
    if (OCR5C == 10) {
      OCR5A = 0;
    }
  } else {
    OCR5C++;
    if (OCR5C == 250) {
      OCR5A = 1;
    }
  }
  sei();
}

void boilTachISR() {
  cli();
  boilTicks++;
  sei();
}

void mashTachISR() {
  cli();
  mashTicks++;
  sei();
}

void drainButtonISR() {
  cli();
  drainButton = true;
  sei();
}

uint8_t TWIReadACK(void) {
    TWCR = (1<<TWINT)|(1<<TWEN)|(1<<TWEA);
    uint32_t timeout = millis();
    while ((TWCR & (1<<TWINT)) == 0) {
      if (millis() - timeout > 20) {
        break;
      }
    }
    return TWDR;
}

uint8_t TWIReadNACK(void) {
    TWCR = (1<<TWINT)|(1<<TWEN);
    uint32_t timeout = millis();
    while ((TWCR & (1<<TWINT)) == 0) {
      if (millis() - timeout > 20) {
        break;
      }
    }
    return TWDR;
}

void TWIWrite(uint8_t u8data) {
    TWDR = u8data;
    TWCR = (1<<TWINT)|(1<<TWEN);
    uint32_t timeout = millis();
    while ((TWCR & (1<<TWINT)) == 0) {
      if (millis() - timeout > 20) {
        break;
      }
    }
}

void TWIStart(void) {
    TWCR = (1<<TWINT)|(1<<TWSTA)|(1<<TWEN);
    while ((TWCR & (1<<TWINT)) == 0);
}
//send stop signal
void TWIStop(void) {
    TWCR = (1<<TWINT)|(1<<TWSTO)|(1<<TWEN);
}

uint8_t TWIGetStatus(void) {
    uint8_t status;
    //mask status
    status = TWSR & 0xF8;
    return status;
}

void TWIInit(void) {
    //set SCL to 400kHz
    TWCR = 0;
    delay(10);
    TWSR = 0x00;
    TWBR = 72;
    //enable TWI
    TWCR = (1<<TWEN);
}

uint8_t PressureReadAll(int16_t *pressure, uint16_t *temperature) {
    TWIStart();
    if (TWIGetStatus() != 0x08)
        return 1;
    //select devise and send A2 A1 A0 address bits
    TWIWrite(massAddress << 1);
    
    if (TWIGetStatus() != 0x18)
        return 1;
    //send the rest of address
    TWIWrite(0);
    
    if (TWIGetStatus() != 0x28)
        return 1;
    //send start
    TWIStart();

    if (TWIGetStatus() != 0x10)
        return 1;
    //select devise and send read bit
    TWIWrite((massAddress << 1)|1);
    
    if (TWIGetStatus() != 0x40)
        return 1;
    *pressure = TWIReadACK() << 8;
    *pressure |= TWIReadACK();
    *temperature = TWIReadACK() << 8;
    *temperature |= TWIReadNACK();
    if (TWIGetStatus() != 0x58)
        return 1;
    TWIStop();
    return 0;
}

uint8_t ScanTWI() {
  uint8_t address = 255;
  for (uint8_t x = 0; x<129; x++) {
    TWIStart();
    //if (TWIGetStatus() != 0x08)
        //return 1;
    //select devise and send A2 A1 A0 address bits
    TWIWrite(x << 1);    
    if (TWIGetStatus() == 0x18) {
      address = x;
      break;
    }
    TWIStop();
  }
  return address;
}