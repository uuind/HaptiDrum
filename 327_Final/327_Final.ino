// --- PIN CONFIGURATION ---
const int analogPin = A0;      // Input signal monitoring
const int audioOutPin = 9;     // Audio filter output -> TPA3116 (+) (Must be Pin 9)
const int i2cPowerPin = 6;        // Vibration motor driver pin (Pin 6)

// --- RELATIVE SPIKE DETECTION SETTINGS ---
// This looks at the instant difference between the current loop and the previous loop
const int SUDDEN_JUMP_THRESHOLD = 150; // Jump required within 10ms to trigger (Adjust this!)
const unsigned int SPIKE_TIMEOUT = 50; // Max allowed spike width in ms before rejection

// --- AUDIO BASS SETTINGS ---
const int BASS_FREQUENCY = 25;        // Pitch of the bass thump in Hz
const float audioDecayRate = 0.95;    // Audio decay factor
const int maxAudioVol = 255;          // Initial audio peak volume

// --- MOTOR SETTINGS ---
const float motorDecayRate = 0.90;    // Motor decay factor
const int maxMotorVibe = 130;         // Initial motor peak strength

// --- STATE TRACKING VARIABLES ---
int lastAnalogReading = 0;             // Stores the snapshot from 10ms ago
unsigned long spikeStartTimeMs = 0;
bool spikeActive = false;
bool spikeRejected = false;            // Latches true if the input stays high too long

// Oscillator clock tracking
const unsigned long halfPeriodUs = 1000000 / BASS_FREQUENCY / 2;
unsigned long lastToggleTimeUs = 0;
bool waveState = false;

// Decay tracking
unsigned long lastDecayTimeMs = 0;
unsigned long lastSampleTimeMs = 0;
float currentAudioVol = 0;
float currentMotorVibe = 0;

void setup() {
  Serial.begin(9600);
  pinMode(audioOutPin, OUTPUT);
  pinMode(i2cPowerPin, OUTPUT);
  
  // HARDWARE REGISTRY HACK (Sets Pin 9 to silent 31.25 kHz PWM)
  TCCR1B = (TCCR1B & 0b11111000) | 0x01;
  
  // Initialize the tracking snapshot
  lastAnalogReading = analogRead(analogPin);
  digitalWrite(i2cPowerPin, HIGH);
  Serial.println("--- Dynamic Rate-of-Change Spike Detector Active ---");
}

void loop() {
  unsigned long currentMillis = millis();
  unsigned long currentMicros = micros();

  // 1. Core Sample Timer (Runs strictly every 10ms to keep our math accurate)
  if (currentMillis - lastSampleTimeMs >= 10) {
    lastSampleTimeMs = currentMillis;
    
    int currentReading = analogRead(analogPin);
    
    // Calculate the instant dynamic jump relative to the immediate past
    int dynamicJump = currentReading - lastAnalogReading;

    // Check for a sudden positive jump
    if (dynamicJump > SUDDEN_JUMP_THRESHOLD) {
      if (!spikeActive && !spikeRejected) {
        // A sudden delta acceleration detected! Fire outputs instantly.
        spikeStartTimeMs = currentMillis;
        spikeActive = true;
        currentAudioVol = maxAudioVol;
        currentMotorVibe = maxMotorVibe;
        Serial.print("!! DYNAMIC JUMP !! Delta: +");
        Serial.println(dynamicJump);
      }
    }

    // Monitor an ongoing spike for timeout rejection
    if (spikeActive) {
      // If the signal remains high relative to where it started, track the duration
      if ((currentMillis - spikeStartTimeMs) > SPIKE_TIMEOUT) {
        // Rejection rule triggered: It stayed high too long. Kill everything.
        currentAudioVol = 0;
        currentMotorVibe = 0;
        spikeActive = false;
        spikeRejected = true; // Block re-triggering until it completely resets
        Serial.println("Sustained input detected. Outputs rejected.");
      }
    }

    // Reset logic: The signal has dropped back down or normalized
    // If the immediate jump is flat or negative, the spike event is over
    if (dynamicJump <= 5 && currentReading < (lastAnalogReading + 10)) {
      if (spikeActive || spikeRejected) {
        spikeActive = false;
        spikeRejected = false; // Arm the system for the next sudden jump
      }
    }

    // Save this loop's reading as the next loop's past reference point
    lastAnalogReading = currentReading;
  }

  // 2. Continuous 60Hz Pitch Oscillator (Unbound by the 10ms timer for audio precision)
  if (currentMicros - lastToggleTimeUs >= halfPeriodUs) {
    lastToggleTimeUs = currentMicros;
    waveState = !waveState; 
  }

  // 3. Independent Decay Loop (Runs every 10ms)
  if (currentMillis - lastDecayTimeMs >= 10) {
    lastDecayTimeMs = currentMillis;
    
    if (currentAudioVol > 1) {
      currentAudioVol = currentAudioVol * audioDecayRate;
    } else {
      currentAudioVol = 0;
    }

    if (currentMotorVibe > 1) {
      currentMotorVibe = currentMotorVibe * motorDecayRate;
    } else {
      currentMotorVibe = 0;
    }
  }

  // 4. Final Hardware Output
  if (waveState) {
    analogWrite(audioOutPin, (int)currentAudioVol);
  } else {
    analogWrite(audioOutPin, 0);
  }
}