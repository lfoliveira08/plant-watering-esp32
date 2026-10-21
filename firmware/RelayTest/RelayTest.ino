// Relay-only test: ON 3 s, OFF 3 s, forever. Same relay logic as RealHardware.ino.
static const int  PIN_RELAY = 26;
static const bool RELAY_ACTIVE_LOW     = false;
static const bool RELAY_FLOAT_WHEN_OFF = false;

static void relayWrite(bool on) {
  if (RELAY_FLOAT_WHEN_OFF && RELAY_ACTIVE_LOW) {
    pinMode(PIN_RELAY, OUTPUT_OPEN_DRAIN);   // LOW = relay ON, HIGH = released (module pull-up = OFF)
    digitalWrite(PIN_RELAY, on ? LOW : HIGH);
    return;
  }
  pinMode(PIN_RELAY, OUTPUT);
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(PIN_RELAY, level ? HIGH : LOW);
}

void setup() { Serial.begin(115200); relayWrite(false); }

void loop() {
  Serial.println("relay ON");  relayWrite(true);  delay(3000);
  Serial.println("relay OFF"); relayWrite(false); delay(3000);
}
