// =====================================================================
//  Haptic Drumset - 4 Drums, 4 ERM Regions, Overlapping Profiles
//                   + Overlapping Bass
//
//  FSRs:        drum 1 -> A0,  drum 2 -> A1,  drum 3 -> A2,  drum 4 -> A3
//  ERM regions: region 1..4, each a contiguous block of PCA9685 channels
//  Bass:        single TT25 on pin 9 -> RC filter -> TPA3116
//
//  Each drum hit starts an INDEPENDENT decaying event for BOTH its ERM
//  envelope and its bass envelope. ERM regions SUM the contributions of
//  all active drums; the single TT25 plays the SUM of all active drums'
//  bass envelopes. Hits in quick succession overlap. Re-hitting the SAME
//  drum refreshes that drum's envelopes to full.
//
//  INVERTED ERM LOGIC: PCA9685 duty 0 = max vibration, 4095 = off.
//
//  WIRING:
//    Hapkit 5V/GND -> PCA9685 VCC/GND    A4 -> SDA   A5 -> SCL
//    PCA9685 V+ (screw terminal) <- motor supply (keep near 5V)
//    ERMs: red on V+ pin, blue on PWM pin
//    FSR dividers -> A0..A3
//  Requires: "Adafruit PWM Servo Driver Library"
// =====================================================================

#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver(0x40);

// #####################################################################
// ##                                                                 ##
// ##                  >>>  TUNING SECTION  <<<                        ##
// ##        Adjust everything in this block to shape the feel.        ##
// ##                                                                 ##
// #####################################################################

// --- NUMBER OF DRUMS / REGIONS (keep both at 4 for this build) -------
const int NUM_DRUMS   = 4;
const int NUM_REGIONS = 4;

// --- WHICH PCA9685 CHANNELS BELONG TO EACH ERM REGION ----------------
//   Each region is a contiguous channel range [lo, hi].
const int REGION_CH_LO[NUM_REGIONS] = {  0,  4,  8, 12 };
const int REGION_CH_HI[NUM_REGIONS] = {  3,  7, 11, 15 };

// --- STRIKE SENSITIVITY (per drum) -----------------------------------
//   ADC jump within one 10 ms sample that counts as a hit.
const int DRUM_THRESHOLD[NUM_DRUMS] = { 150, 150, 150, 150 };

// --- ERM REGION PROFILE PER DRUM -------------------------------------
//   ERM_PROFILE[drum][region] = 0..255 peak strength this drum sends to
//   that region. 0 = region untouched by this drum.
//
//                              region:  1    2    3    4
const int ERM_PROFILE[NUM_DRUMS][NUM_REGIONS] = {
  /* drum 1 */                        { 255, 0,   0,   0 },
  /* drum 2 */                        {   0, 255, 0,   0 },
  /* drum 3 */                        {   0,   0, 255, 0 },
  /* drum 4 */                        { 0,   0,   0, 255 },
};

// --- ERM DECAY RATE PER DRUM -----------------------------------------
//   Multiplier per 10 ms. ~0.85 snappy, ~0.95 medium, ~0.98 long.
const float ERM_DECAY[NUM_DRUMS] = { 0.95, 0.90, 0.92, 0.97 };

// --- BASS PROFILE PER DRUM -------------------------------------------
//   BASS_PEAK  = 0..255 peak bass amplitude this drum contributes.
//   BASS_DECAY = decay multiplier per 10 ms (closer to 1.0 = longer).
//   The single TT25 plays the SUM of all active drums' bass envelopes,
//   so two quick hits thump harder and longer than one.
const int   BASS_PEAK [NUM_DRUMS] = { 255, 220,  200, 180 };
const float BASS_DECAY[NUM_DRUMS] = { 0.96, 0.94, 0.92, 0.97 };

// --- BASS PITCH ------------------------------------------------------
const int BASS_FREQUENCY = 25;        // Hz - pitch of the bass thump

// --- ERM OUTPUT FLOOR ------------------------------------------------
//   Region levels below this snap to 0 (kills ERM coast-down tail).
const int ERM_CUTOFF = 40;

// --- STRIKE TIMING ---------------------------------------------------
const unsigned int SPIKE_TIMEOUT = 50;   // ms; sustained press rejected after this

// #####################################################################
// ##                  >>>  END TUNING SECTION  <<<                    ##
// #####################################################################


// --- PIN / HARDWARE CONSTANTS ----------------------------------------
const int audioOutPin = 9;            // bass out (must be pin 9)
const int PCA_DUTY_MAX = 4095;
const int ERM_OFF_DUTY = 4095;        // inverted logic: 4095 = off

const int DRUM_PIN[NUM_DRUMS] = { A0, A1, A2, A3 };

// --- OSCILLATOR ------------------------------------------------------
const unsigned long halfPeriodUs = 1000000UL / BASS_FREQUENCY / 2;
unsigned long lastToggleTimeUs = 0;
bool waveState = false;

// --- TIMING ----------------------------------------------------------
unsigned long lastDecayTimeMs  = 0;
unsigned long lastSampleTimeMs = 0;

// =====================================================================
//  Per-drum state. Each drum carries its OWN independent ERM envelope
//  AND its own independent bass envelope, so multiple drums can be
//  "alive" and decaying at the same time for both outputs.
// =====================================================================
struct Drum {
  int  analogPin;
  int  lastReading;
  unsigned long spikeStartMs;
  bool spikeActive;
  bool spikeRejected;

  float ermLevel;     // 0..255 - this drum's ERM envelope
  float bassLevel;    // 0..255 - this drum's bass envelope

  int  strikeForce; 
};

Drum drums[NUM_DRUMS];

void setup() {
  Serial.begin(9600);
  pinMode(audioOutPin, OUTPUT);

  // Pin 9 -> ~31.25 kHz PWM
  TCCR1B = (TCCR1B & 0b11111000) | 0x01;

  Wire.begin();
  pwm.begin();
  pwm.setPWMFreq(1000);
  for (int ch = 0; ch < 16; ch++) pwm.setPWM(ch, 0, ERM_OFF_DUTY);

  for (int i = 0; i < NUM_DRUMS; i++) {
    drums[i].analogPin     = DRUM_PIN[i];
    drums[i].lastReading   = analogRead(DRUM_PIN[i]);
    drums[i].spikeStartMs  = 0;
    drums[i].spikeActive   = false;
    drums[i].spikeRejected = false;
    drums[i].ermLevel      = 0;
    drums[i].bassLevel     = 0;
  }

  Serial.println("--- 4-Drum Haptic Drumset Active (overlapping bass) ---");
}

// Write a 0-255 level to one ERM region, applying inverted duty.
void writeRegion(int region, int level255) {
  if (level255 < 0)   level255 = 0;
  if (level255 > 255) level255 = 255;
  if (level255 < ERM_CUTOFF) level255 = 0;        // snap-to-off floor

  int duty = PCA_DUTY_MAX - (level255 * PCA_DUTY_MAX / 255);
  for (int ch = REGION_CH_LO[region]; ch <= REGION_CH_HI[region]; ch++) {
    pwm.setPWM(ch, 0, duty);
  }
}

// Strike detection for one drum. Returns true on a NEW strike.
bool detectStrike(int i) {
  Drum &d = drums[i];
  int reading = analogRead(d.analogPin);
  int jump = reading - d.lastReading;
  bool fired = false;

  if (jump > DRUM_THRESHOLD[i]) {
    if (!d.spikeActive && !d.spikeRejected) {
      d.spikeStartMs = millis();
      d.spikeActive  = true;
      d.strikeForce  = jump; 
      fired = true;
    }
  }

  if (d.spikeActive) {
    if ((millis() - d.spikeStartMs) > SPIKE_TIMEOUT) {
      d.spikeActive   = false;
      d.spikeRejected = true;
    }
  }

  if (jump <= 5 && reading < (d.lastReading + 10)) {
    if (d.spikeActive || d.spikeRejected) {
      d.spikeActive   = false;
      d.spikeRejected = false;
    }
  }

  d.lastReading = reading;
  return fired;
}

void loop() {
  unsigned long currentMillis = millis();
  unsigned long currentMicros = micros();

  // -------------------------------------------------------------------
  // 1. Strike detection for all 4 drums (every 10 ms)
  // -------------------------------------------------------------------
  if (currentMillis - lastSampleTimeMs >= 10) {
    lastSampleTimeMs = currentMillis;

    for (int i = 0; i < NUM_DRUMS; i++) {
      if (detectStrike(i)) {
        // New hit on drum i: refresh BOTH of this drum's envelopes.
        drums[i].ermLevel  = 255;            // ERM envelope to full
        drums[i].bassLevel = BASS_PEAK[i];   // bass envelope to this drum's peak

        Serial.print("DRUM,");
        Serial.print(i + 1);
        Serial.print(",");
        Serial.println(drums[i].strikeForce);
      }
    }
  }

  // -------------------------------------------------------------------
  // 2. Bass pitch oscillator (microsecond-timed)
  // -------------------------------------------------------------------
  if (currentMicros - lastToggleTimeUs >= halfPeriodUs) {
    lastToggleTimeUs = currentMicros;
    waveState = !waveState;
  }

  // -------------------------------------------------------------------
  // 3. Decay loop (every 10 ms) - each drum's ERM AND bass envelope
  //    decays independently, at its own per-drum rate.
  // -------------------------------------------------------------------
  if (currentMillis - lastDecayTimeMs >= 10) {
    lastDecayTimeMs = currentMillis;

    for (int i = 0; i < NUM_DRUMS; i++) {
      if (drums[i].ermLevel > 1) drums[i].ermLevel *= ERM_DECAY[i];
      else                       drums[i].ermLevel = 0;

      if (drums[i].bassLevel > 1) drums[i].bassLevel *= BASS_DECAY[i];
      else                        drums[i].bassLevel = 0;
    }
  }

  // -------------------------------------------------------------------
  // 4. Hardware output
  // -------------------------------------------------------------------
  // Bass: SUM every active drum's bass envelope, clamp, gate the square
  // wave. Overlapping hits add amplitude; each tail decays on its own.
  float bassSum = 0;
  for (int i = 0; i < NUM_DRUMS; i++) {
    bassSum += drums[i].bassLevel;
  }
  if (bassSum > 255) bassSum = 255;            // clamp the sum

  if (waveState) analogWrite(audioOutPin, (int)bassSum);
  else           analogWrite(audioOutPin, 0);

  // ERM regions: SUM every active drum's contribution into each region.
  for (int r = 0; r < NUM_REGIONS; r++) {
    float regionSum = 0;
    for (int i = 0; i < NUM_DRUMS; i++) {
      regionSum += drums[i].ermLevel * (ERM_PROFILE[i][r] / 255.0);
    }
    if (regionSum > 255) regionSum = 255;      // clamp the sum
    writeRegion(r, (int)regionSum);
  }
}

// =====================================================================
//  HOW TO TUNE
//  -------------------------------------------------------------------
//  ERM_PROFILE[drum][region]   - main "feel" table. Row = drum,
//      column = ERM region, value 0-255 = drive strength at peak.
//  ERM_DECAY[drum]             - per-drum buzz length.
//  BASS_PEAK[drum]             - per-drum bass thump strength.
//  BASS_DECAY[drum]            - per-drum bass tail length.
//  REGION_CH_LO/HI             - PCA9685 channels per ERM region.
//  DRUM_THRESHOLD[drum]        - per-drum hit sensitivity.
//
//  OVERLAP BEHAVIOUR
//   Every drum has its OWN ERM envelope and its OWN bass envelope.
//   - ERM regions sum all active drums' contributions.
//   - The single TT25 plays the sum of all active drums' bass
//     envelopes. Quick successive hits overlap (bigger, longer thump).
//   - Re-hitting the SAME drum refreshes that drum's envelopes.
//
//  BASS CAVEAT (important):
//   There is ONE TT25 and ONE oscillator at ONE pitch (BASS_FREQUENCY).
//   Summing bass envelopes makes the shared thump LOUDER and LONGER on
//   overlap - it does NOT play two different bass pitches at once. For
//   genuinely distinct simultaneous bass pitches you would need
//   per-drum frequencies and sine-table mixing (a larger change).
//
//  OTHER NOTES
//   - writeRegion() writes all 16 channels every loop. If the bass
//     sounds weak, that I2C traffic is slowing loop() - ask for the
//     "write only on change" optimisation.
//   - ERMs on bare PCA pins are current-limited and creep if V+ rises
//     much above 5V. A ULN2803 driver fixes both; keep V+ ~5V for now.
// =====================================================================
