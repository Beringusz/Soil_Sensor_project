#include <LiquidCrystal.h>

LiquidCrystal lcd(7, 8, 9, 10, 11, 12);
//for imitating modbus library
#define BAUD 9600 //this is the baud rate
#define MYUBRR (16000000UL/16/BAUD-1)//16Mz / 16/ (BAUD - 1)

// offset values
float tempOffset = 0.0;
float moistureOffset = 0.0;
float ecMultiplier = 1.0;
int ecMin = 200, ecMax = 2000;
int phMin = 60, phMax = 75;

//Calibration coeff for NPK
const float beta0 = 5.0;   //Baseline intercept
const float beta1 = 0.04;  //EC weight
const float beta2 = 0.5;   //Moisture weight
const float beta3 = 0.001; //Interaction weight (EC * Moisture)

//vars for storing data, initialized
float currentTemp = 0.0;
float currentMoisture = 0.0;
float currentEc = 0.0;
float currentPh = 0.0;

//Factory NPK
uint16_t currentN = 0;
uint16_t currentP = 0;
uint16_t currentK = 0;

//Custom NPK
float calcN = 0.0;
float calcP = 0.0;
float calcK = 0.0;

//for LCD display
int displayState = 0;
int maxStates = 4;
unsigned long lastSensorRead = 0;
const unsigned long readInterval = 3000;

//button debouncing variables
int buttonState = HIGH;
bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

//UART init function
void UART_Init(unsigned int ubrr) 
{
  UBRR0H = (unsigned char)(ubrr >> 8); 
  UBRR0L = (unsigned char)ubrr; 
  UCSR0B = (1 << RXEN0) | (1 << TXEN0); 
  UCSR0C = (1 << UCSZ01) | (1 << UCSZ00); 
}

//wait until operation is complete, then send the data
void UART_Transmit(unsigned char data) 
{
  while (!(UCSR0A & (1 << UDRE0))); 
  UDR0 = data; 
}

//wait until operation is complete, then receive the data and return it
int UART_Receive(uint16_t timeout_ms) 
{
  uint32_t start = millis(); 
  while (!(UCSR0A & (1 << RXC0))) 
  {
    if (millis() - start > timeout_ms) return -1; 
  }
  return UDR0; 
}

//cyclic redundancy check CRC16 for Modbus variant
uint16_t ModbusCRC16(uint8_t *buf, uint8_t len)
{
  uint16_t crc = 0xFFFF; 
  for (uint8_t pos = 0; pos < len; pos++) 
  {
    crc ^= (uint16_t)buf[pos]; 
    for (int i = 8; i != 0; i--) 
    {
      if ((crc & 0x0001) != 0) 
      {
        crc >>= 1;
        crc ^= 0xA001; 
      } else 
      {
        crc >>= 1;
      }
    }
  }
  return crc;
}

void setup() 
{
  //read button state
  DDRD &= ~(1 << DDD4);   
  PORTD |= (1 << PORTD4); 
  
  //uart init
  UART_Init(MYUBRR);
  //lcd init
  lcd.begin(16, 2);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("System Booting..");

  delay(2000); 
  updateDisplay();
}

void loop() {
  // button debouncing
  bool isPressed = !(PIND & (1 << PIND4));
  bool reading = isPressed ? LOW : HIGH;

  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }

  //update display, if button is pressed and released
  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) { 
        displayState++;
        if (displayState >= maxStates) displayState = 0; 
        updateDisplay();
      }
    }
  }
  
  lastButtonState = reading; 

  if (millis() - lastSensorRead >= readInterval) {
    lastSensorRead = millis();
    readSensor();
  }
}

//reading the sensor from 0x02 addr
void readSensor() 
{
  uint8_t requestFrame[8] = {0x02, 0x03, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00}; 
  uint16_t crc = ModbusCRC16(requestFrame, 6); 
  requestFrame[6] = crc & 0xFF; 
  requestFrame[7] = (crc >> 8) & 0xFF;

  while (UCSR0A & (1 << RXC0)) 
  { 
    char dump = UDR0; 
  }

  for (int i = 0; i < 8; i++) 
  {
    UART_Transmit(requestFrame[i]); 
  }

  uint8_t response[19]; 
  bool success = true; 

  for (int i = 0; i < 19; i++) 
  {
    int incomingByte = UART_Receive(200); 
    if (incomingByte == -1) {
      success = false;
      break; 
    }
    response[i] = (uint8_t)incomingByte;
  }

  if (success && response[0] == 0x02 && response[1] == 0x03) 
  {
    uint16_t rawTemp     = (response[3] << 8) | response[4]; 
    uint16_t rawMoisture = (response[5] << 8) | response[6];
    uint16_t rawEc       = (response[7] << 8) | response[8];
    uint16_t rawN        = (response[11] << 8) | response[12];
    uint16_t rawP        = (response[13] << 8) | response[14];
    uint16_t rawK        = (response[15] << 8) | response[16];

    currentTemp = ((int16_t)rawTemp / 10.0) + tempOffset; 
    currentMoisture = (rawMoisture / 10.0) + moistureOffset;
    currentEc = rawEc * ecMultiplier;

    int constrainedEc = constrain(currentEc, ecMin, ecMax);
    currentPh = map(constrainedEc, ecMin, ecMax, phMin, phMax) / 10.0;

    currentN = rawN;
    currentP = rawP;
    currentK = rawK;

    //calculate Standardized EC at 25C (alpha = 0.019)
    float ec25 = currentEc / (1.0 + 0.019 * (currentTemp - 25.0));

    //apply Multiple Linear Regression for Nitrogen
    calcN = beta0 + (beta1 * ec25) + (beta2 * currentMoisture) + (beta3 * ec25 * currentMoisture);

    //proportional Estimation for P and K (Typical Agronomic Ratios)
    calcP = calcN * 0.25; //just an example
    calcK = calcN * 0.60; //just an example

    updateDisplay();
  }
}

//LCD setup for display
void updateDisplay() {
  lcd.clear();
  switch (displayState) {
    case 0:
      lcd.setCursor(0, 0);
      lcd.print("Temp: "); lcd.print(currentTemp, 1); lcd.print(" C");
      lcd.setCursor(0, 1);
      lcd.print("Moist: "); lcd.print(currentMoisture, 1); lcd.print(" %");
      break;
    case 1:
      lcd.setCursor(0, 0);
      lcd.print("EC: "); lcd.print((int)currentEc); lcd.print(" us/cm");
      lcd.setCursor(0, 1);
      lcd.print("pH: "); lcd.print(currentPh, 1);
      break;
    case 2:
      //Factory Estimated NPK
      lcd.setCursor(0, 0);
      lcd.print("fN:"); lcd.print(currentN);
      lcd.print(" fP:"); lcd.print(currentP);
      lcd.setCursor(0, 1);
      lcd.print("fK:"); lcd.print(currentK); lcd.print(" mg/kg");
      break;
    case 3:
      //Custom Algorithmic NPK
      lcd.setCursor(0, 0);
      lcd.print("cN:"); lcd.print((int)calcN);
      lcd.print(" cP:"); lcd.print((int)calcP);
      lcd.setCursor(0, 1);
      lcd.print("cK:"); lcd.print((int)calcK); lcd.print(" mg/kg");
      break;
  }
}