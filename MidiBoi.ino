#include <TM1637Display.h>
#include <EEPROM.h>
#include <OneButton.h>
#include <RotaryEncoder.h>
#include <util/atomic.h>
#include <Control_Surface.h> // Version pinned in sketch.yaml

// ------------------------------------------------------------------ pins --

constexpr uint8_t PIN_DISPLAY_CLK = 14;
constexpr uint8_t PIN_DISPLAY_DIO = 15;
constexpr uint8_t PIN_ENCODER_A   = 2;
constexpr uint8_t PIN_ENCODER_B   = 3;
constexpr uint8_t PIN_ENCODER_SW  = 4;

// ---------------------------------------------------------------- limits --

// Control Surface documents the usable controller number range as [0, 119].
// 120-127 are Channel Mode messages (All Sound Off, Reset All Controllers,
// All Notes Off, ...) and sweeping a modwheel through those would badly
// misbehave on the receiving instrument. Raise to 127 if you really want them.
constexpr uint8_t CC_MAX    = 119;
constexpr uint8_t VALUE_MAX = 127;

// ---------------------------------------------------------- ranged sender --

/**
 * Rescales the 7-bit potentiometer value into an arbitrary [lower, upper]
 * output window. Despite the name it no longer sends anything -- see
 * RangedCCPotentiometer, which owns the send decision so that it can be made
 * after this scaling rather than before it.
 *
 * Why here and not via FilteredAnalog::map(): Control Surface's map() takes a
 * plain function pointer (`analog_t (*)(analog_t)`), so the mapping function
 * cannot carry any per-potentiometer state -- which is what forced the earlier
 * template-per-index workaround. This, by contrast, is an ordinary per-element
 * object, so limits stored in it are freely writable at runtime.
 */
class RangedCCSender {
  public:
    /// User-facing output window, edited from the front panel.
    uint8_t lower = 0;
    uint8_t upper = VALUE_MAX;

    constexpr static uint8_t precision() { return 7; }

    /// Map the full travel onto lower..upper.
    /// `upper < lower` is legal and simply inverts the response.
    /// Input calibration happens earlier, see calibratePot().
    uint8_t scale(uint8_t value) const {
        int16_t span = int16_t(upper) - int16_t(lower); // -127 .. 127
        int16_t num = int16_t(value) * span;            // |num| <= 16129
        int16_t step = num >= 0 ? (num + 63) / 127 : (num - 63) / 127;
        return uint8_t(int16_t(lower) + step);
    }
};

/// A CCPotentiometer whose controller number *and* output range can both be
/// changed at runtime -- no Banks required.
///
/// This owns its FilteredAnalog instead of inheriting MIDIFilteredAnalog, for
/// one reason: MIDIFilteredAnalog::update() is `final`, and it sends whenever
/// the *pre-scaling* hysteresis moves. RangedCCSender::scale() then compresses
/// that 7-bit value into [lower, upper], so a narrow window maps several input
/// steps onto one output value -- and every one of them still emitted a CC
/// message carrying a value identical to the previous one. Measured with
/// L=32/H=63, roughly three messages in four were duplicates. Deciding after
/// scaling fixes it, and `final` leaves no way to do that by inheritance.
class RangedCCPotentiometer : public MIDIOutputElement {
  public:
    RangedCCPotentiometer(pin_t analogPin, MIDIAddress address)
        : filteredAnalog(analogPin), address(address) {}

    void begin() override {
        filteredAnalog.resetToCurrentValue();
        // Seed the duplicate check with where the wheel actually is, so the
        // first movement is compared against reality rather than a phantom 0.
        lastSent = sender.scale(filteredAnalog.getValue());
    }

    void update() override {
        if (filteredAnalog.update())
            sendIfChanged();
    }

    /// Send only when the value that would go on the wire has actually moved.
    void sendIfChanged() {
        uint8_t value = sender.scale(filteredAnalog.getValue());
        if (value == lastSent)
            return;
        lastSent = value;
        Control_Surface.sendControlChange(address, value);
    }

    /// Send regardless of whether the value changed. Used when the CC number
    /// or the output window was edited underneath us, where the instrument
    /// needs the current value even though the wheel has not moved.
    void forcedUpdate() {
        lastSent = sender.scale(filteredAnalog.getValue());
        Control_Surface.sendControlChange(address, lastSent);
    }

    void map(MappingFunction fn) { filteredAnalog.map(fn); }
    void setAddress(MIDIAddress addr) { address = addr; }
    MIDIAddress getAddress() const { return address; }

    static constexpr analog_t getMaxRawValue() {
        return FilteredAnalog_t::getMaxRawValue();
    }

    /// User-facing output window, edited from the front panel.
    RangedCCSender sender;

  private:
    using FilteredAnalog_t = AH::FilteredAnalog<RangedCCSender::precision()>;
    FilteredAnalog_t filteredAnalog;
    MIDIAddress address;
    /// 0xFF is not a legal 7-bit value, so it reads as "nothing sent yet".
    uint8_t lastSent = 0xFF;
};

// ------------------------------------------------------------- hardware ----

TM1637Display display(PIN_DISPLAY_CLK, PIN_DISPLAY_DIO);
RotaryEncoder encoder(PIN_ENCODER_A, PIN_ENCODER_B);
OneButton enc_button(PIN_ENCODER_SW, true);

// Two MIDI interfaces, joined to Control Surface through a pipe factory
USBMIDI_Interface usbmidi;
HardwareSerialMIDI_Interface serialmidi = {Serial1, MIDI_BAUD};
BidirectionalMIDI_PipeFactory<2> pipes;

constexpr uint8_t NUM_POTS = 2;

// TODO: fix the PCB design, only pins A1 and A9 are functional on the pcb,
// some headers are wrongly connected to PWM pins instead of Analog.
// Investigate the board with preferably another Arduino.
RangedCCPotentiometer potentiometers[NUM_POTS] = {
    {A0, {26, Channel_1}},
    {A9, {24, Channel_1}},
};

// Full-scale value Control Surface hands to the mapping function: the 10-bit
// ADC reading, widened to 14 bits before filtering and hysteresis.
constexpr analog_t RAW_MAX = RangedCCPotentiometer::getMaxRawValue();

// Per-unit calibration: {inLow, inHigh} of each wheel's usable travel, in raw
// 14-bit units. The bounds sit inside the measured extremes and calibrate()
// clamps outside them. That clamp is the only reason 0 and 127 are reachable
// at all: Hysteresis<7> uses a margin of 127 with an offset of 64, so stepping
// onto the last value needs the input within 64 counts of the rail, which a
// hand-moved wheel will not deliver reliably.
//
// Measured from the raw ADC (2026-11-04), normal playing force, several
// stop-to-stop sweeps, in 10-bit counts -- NOT through a MIDI monitor, which
// reports post-hysteresis values and therefore reads systematically narrower
// than the true travel:
//
//   A0  travel 52..1023   bottom of travel lands 52..64, worst case 72
//   A9  travel  0..996
//
// A0's lower bound was 3 * 128 (ADC 24), below the bottom of travel, so the
// wheel could never reach the clamp. Its lowest 7-bit output was 4, which
// through an output window of L=64/H=127 showed up as 66 instead of 64. It is
// now 12 * 128 (ADC 96), three MIDI steps clear of the worst-case push.
//
// These shift when a wheel is refitted: removing both pots and putting them
// back moved A0's bottom from 0 to 52 and A9's top from 957 to 996, because
// the wheel ends up at a different angle on the shaft. Re-measure after any
// mechanical work, and prefer the raw ADC over a MIDI monitor when doing so.
const analog_t POT_CALIBRATION[NUM_POTS][2] = {
    {12 * 128, 123 * 128}, // A0
    {9 * 128, 108 * 128},  // A9, both ends verified to reach their clamps
};

/// Stretch a pot's usable travel back out to the full raw range, so the
/// quantisation to 7 bits downstream still yields all 128 values.
analog_t calibrate(analog_t raw, uint8_t pot) {
    const analog_t lo = POT_CALIBRATION[pot][0];
    const analog_t hi = POT_CALIBRATION[pot][1];
    if (raw <= lo)
        return 0;
    if (raw >= hi)
        return RAW_MAX;
    return analog_t((uint32_t(raw - lo) * RAW_MAX) / (hi - lo));
}

// FilteredAnalog::map() takes a bare `analog_t (*)(analog_t)`, so it cannot be
// told at runtime which pot it is working for. Calibration *is* fixed per pot,
// though, so a template stamping out one stateless function per index is
// exactly the right tool -- unlike the output range, which the user edits and
// therefore lives in RangedCCSender instead.
template <uint8_t Pot>
analog_t calibratePot(analog_t raw) {
    return calibrate(raw, Pot);
}

const MappingFunction POT_MAPPING[NUM_POTS] = {calibratePot<0>, calibratePot<1>};

// -------------------------------------------------------------- settings ---

struct PotConfig {
    uint8_t cc;
    uint8_t lower;
    uint8_t upper;
};

// Used when the EEPROM holds nothing we recognise.
const PotConfig DEFAULT_CONFIG[NUM_POTS] = {
    {26, 0, VALUE_MAX},
    {24, 0, VALUE_MAX},
};

PotConfig config[NUM_POTS];

// EEPROM layout: [magic][version][pot count] then {cc, lower, upper} per pot.
constexpr uint8_t EEPROM_MAGIC   = 0xA5;
constexpr uint8_t EEPROM_VERSION = 0x01;
constexpr int     EEPROM_DATA    = 3;

int potAddr(uint8_t pot, uint8_t field) { return EEPROM_DATA + pot * 3 + field; }

void loadConfig() {
    for (uint8_t i = 0; i < NUM_POTS; ++i)
        config[i] = DEFAULT_CONFIG[i];

    if (EEPROM.read(0) == EEPROM_MAGIC && EEPROM.read(1) == EEPROM_VERSION &&
        EEPROM.read(2) == NUM_POTS) {
        for (uint8_t i = 0; i < NUM_POTS; ++i) {
            uint8_t cc    = EEPROM.read(potAddr(i, 0));
            uint8_t lower = EEPROM.read(potAddr(i, 1));
            uint8_t upper = EEPROM.read(potAddr(i, 2));
            if (cc <= CC_MAX && lower <= VALUE_MAX && upper <= VALUE_MAX) {
                config[i].cc    = cc;
                config[i].lower = lower;
                config[i].upper = upper;
            }
        }
        return;
    }

    // Pre-1.0 layout: two bare CC numbers at addresses 0 and 1, no ranges.
    const uint8_t legacy[2] = {EEPROM.read(0), EEPROM.read(1)};
    for (uint8_t i = 0; i < NUM_POTS && i < 2; ++i)
        if (legacy[i] <= CC_MAX)
            config[i].cc = legacy[i];
}

void saveConfig() {
    EEPROM.update(0, EEPROM_MAGIC);
    EEPROM.update(1, EEPROM_VERSION);
    EEPROM.update(2, NUM_POTS);
    for (uint8_t i = 0; i < NUM_POTS; ++i) {
        EEPROM.update(potAddr(i, 0), config[i].cc);
        EEPROM.update(potAddr(i, 1), config[i].lower);
        EEPROM.update(potAddr(i, 2), config[i].upper);
    }
}

/// Push a pot's stored settings into the live Control Surface element.
void applyConfig(uint8_t pot) {
    potentiometers[pot].setAddress({config[pot].cc, Channel_1});
    potentiometers[pot].sender.lower = config[pot].lower;
    potentiometers[pot].sender.upper = config[pot].upper;
}

// --------------------------------------------------------------- display ---

const uint8_t SEG_PLAY[] = {
    SEG_A | SEG_B | SEG_E | SEG_F | SEG_G,          // P
    SEG_D | SEG_E | SEG_F,                          // L
    SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G,  // A
    SEG_B | SEG_C | SEG_D | SEG_F | SEG_G,          // Y
};

const uint8_t SEG_SAVE[] = {
    SEG_A | SEG_C | SEG_D | SEG_F | SEG_G,          // S
    SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G,  // A
    SEG_C | SEG_D | SEG_E,                          // v
    SEG_A | SEG_D | SEG_E | SEG_F | SEG_G,          // E
};

// Leading character that tells you which config mode you are in.
constexpr uint8_t CHAR_POT   = SEG_A | SEG_B | SEG_E | SEG_F | SEG_G;  // P
constexpr uint8_t CHAR_CC    = SEG_A | SEG_D | SEG_E | SEG_F;          // C
constexpr uint8_t CHAR_LOWER = SEG_D | SEG_E | SEG_F;                  // L
constexpr uint8_t CHAR_UPPER = SEG_B | SEG_C | SEG_E | SEG_F | SEG_G;  // H

/// Render `label` in the leftmost digit and `value` right-aligned in the rest.
void showLabelled(uint8_t label, uint8_t value) {
    uint8_t seg[4] = {label, 0, 0, 0};
    if (value >= 100)
        seg[1] = display.encodeDigit(value / 100);
    if (value >= 10)
        seg[2] = display.encodeDigit((value / 10) % 10);
    seg[3] = display.encodeDigit(value % 10);
    display.setSegments(seg, 4, 0);
}

// ----------------------------------------------------------------- modes ---

// One encoder click advances through this cycle and wraps back to MODE_PLAY.
enum Mode : uint8_t {
    MODE_PLAY,
    MODE_SELECT_POT,
    MODE_SET_CC,
    MODE_SET_LOWER,
    MODE_SET_UPPER,
    MODE_COUNT,
};

Mode mode = MODE_PLAY;
uint8_t selectedPot = 0; // 0-based; displayed 1-based

constexpr unsigned long SAVE_MESSAGE_MS = 1000;
bool saveMessageActive = false;
unsigned long saveMessageStart = 0;

// ---------------------------------------------------------------- encoder --

// Two problems, two fixes, and they are easy to confuse.
//
// 1. Timing. The display is bit-banged, so showLabelled() blocks for several
//    milliseconds, and a polled decoder loses every edge that lands inside a
//    redraw. Fixed by calling tick() from an ISR on both pins: pins 2 and 3 are
//    INT1/INT0 on the ATmega32U4, so both channels get a real hardware
//    interrupt and the main loop's latency stops mattering.
//
// 2. Contact bounce. Fixed by *which* decoder runs in that ISR, not by the
//    interrupt, and not by sampling faster. Control Surface bundles AHEncoder
//    (a PJRC Encoder derivative);
//    its state table answers a transition where both pins appear to have
//    changed with +/-2, commented "assume pin1 edges only" -- a guess. On a
//    bouncing contact that guess fires constantly and injects phantom counts in
//    whichever direction it picked. Measured on this unit: a slow, deliberate
//    10-detent sweep logged 6 bad steps in 19, including backwards ones.
//    mathertel's equivalent table entries are 0, so an untrackable transition
//    is discarded, and the external position only advances at a detent latch,
//    so mid-travel bounce cannot produce output at all.
//
// 3. Sampling strategy. Pin-change interrupts turned out to be the wrong tool
//    for problem 2: the ISR fires on the chatter itself and reads the pins
//    exactly while they are unstable. Measured that way, a slow 10-detent sweep
//    still produced persistent +/-1 jitter. Sampling at a FIXED 1 kHz in a
//    timer ISR instead is deaf to any bounce shorter than the sample interval,
//    while still being immune to how long the main loop blocks -- which is what
//    problem 1 needed. A quarter-step of a hand-turned encoder lasts several
//    milliseconds, so 1 kHz oversamples real motion comfortably.
//
// An RC filter on A and B remains the real cure for the bounce; this is the
// best the firmware can do on its own.

/// Timer3 compare-match: sample the encoder at a fixed 1 kHz.
/// Timer3 drives only pin 5's PWM on this board, which the sketch does not use.
ISR(TIMER3_COMPA_vect) { encoder.tick(); }

/// Start the sampling timer. 16 MHz / 64 / 250 = 1000 Hz.
///
/// Rate is the one real tuning knob here, and faster is NOT better: measured on
/// this unit, 1 kHz gave a flawless trace (13 clean detents, no reversals)
/// while 2 kHz registered only 6 of 20 slow detents and brought the jitter
/// back. A higher rate captures more of the contact bounce, so the internal
/// quarter-step count gets corrupted mid-detent and `_position >> 2` can land
/// back on the value it started from, absorbing a real detent. Do not raise
/// this without re-measuring; fit the RC filter instead.
void encoderTimerBegin() {
    TCCR3A = 0;
    TCCR3B = (1 << WGM32) | (1 << CS31) | (1 << CS30); // CTC, prescaler 64
    OCR3A = 249;
    TIMSK3 = (1 << OCIE3A);
}

/// getPosition() returns a long that the ISR writes, so a plain read can tear
/// across the four bytes on an 8-bit core.
long encoderPosition() {
    long p;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { p = encoder.getPosition(); }
    return p;
}

/// Encoder position already accounted for. Counted in detents: the library
/// divides the quadrature count by four itself.
long encoderBase = 0;

/// Discard rotation accumulated so far.
void encoderZero() { encoderBase = encoderPosition(); }

/// Detents turned since the last call.
int8_t encoderDelta() {
    long now = encoderPosition();
    long detents = now - encoderBase;
    encoderBase = now;
    if (detents > 127)
        detents = 127;
    else if (detents < -127)
        detents = -127;
    return int8_t(detents);
}

/// Paint the display for the mode we just entered. Rotation from the previous
/// mode is dropped so the knob never jumps a value on entry.
void enterMode() {
    encoderZero();

    switch (mode) {
        case MODE_PLAY:
            for (uint8_t i = 0; i < NUM_POTS; ++i) {
                applyConfig(i);
                potentiometers[i].forcedUpdate(); // resend under the new mapping
            }
            display.setSegments(SEG_PLAY, 4, 0);
            break;
        case MODE_SELECT_POT:
            showLabelled(CHAR_POT, selectedPot + 1);
            break;
        case MODE_SET_CC:
            showLabelled(CHAR_CC, config[selectedPot].cc);
            break;
        case MODE_SET_LOWER:
            showLabelled(CHAR_LOWER, config[selectedPot].lower);
            break;
        case MODE_SET_UPPER:
            showLabelled(CHAR_UPPER, config[selectedPot].upper);
            break;
        default:
            break;
    }
}

void onClick() {
    mode = Mode((mode + 1) % MODE_COUNT);
    saveMessageActive = false;
    enterMode();
}

void onDoubleClick() {
    if (mode != MODE_PLAY)
        return;
    saveConfig();
    display.setSegments(SEG_SAVE, 4, 0);
    saveMessageActive = true;
    saveMessageStart = millis();
}

/// Apply encoder movement to `current`, clamp to [lo, hi], repaint on change.
/// Relative rather than absolute, so the encoder never needs re-seeding and
/// clamping at a limit cannot desync it from the displayed value.
uint8_t editValue(uint8_t current, uint8_t lo, uint8_t hi, uint8_t label) {
    int8_t delta = encoderDelta();
    if (delta == 0)
        return current;

    int16_t value = int16_t(current) + delta;
    if (value < lo)
        value = lo;
    else if (value > hi)
        value = hi;

    if (uint8_t(value) != current)
        showLabelled(label, uint8_t(value));
    return uint8_t(value);
}

// ------------------------------------------------------------------ main ---

void setup() {
    display.setBrightness(0x0b);

    loadConfig();
    for (uint8_t i = 0; i < NUM_POTS; ++i) {
        potentiometers[i].map(POT_MAPPING[i]);
        applyConfig(i);
    }

    // The RotaryEncoder constructor already enabled the pull-ups. Sampling runs
    // off a timer, so no edge depends on the main loop getting round to polling.
    encoderTimerBegin();
    encoderZero();

    enc_button.setDebounceMs(30);
    enc_button.setClickMs(175); // fast timing, subject to experimentation
    enc_button.setPressMs(500);
    enc_button.attachClick(onClick);
    // The switch on this unit is intermittent: it has been measured both
    // working normally and reading open for the better part of an hour, with no
    // repair in between. Binding a long press to onClick as well means any
    // press duration advances the mode, so a contact that only closes under
    // sustained pressure still works. Exactly one of the two handlers fires per
    // press and nothing else is bound to a long press, so this costs no
    // behaviour. Drop it if the switch is ever properly repaired.
    enc_button.attachLongPressStart(onClick);
    enc_button.attachDoubleClick(onDoubleClick);

    Serial1.begin(MIDI_BAUD);
    Control_Surface | pipes | usbmidi;
    Control_Surface | pipes | serialmidi;
    Control_Surface.begin();

    display.setSegments(SEG_PLAY, 4, 0);
}

void loop() {
    enc_button.tick();

    switch (mode) {
        case MODE_PLAY:
            Control_Surface.loop();
            if (saveMessageActive &&
                millis() - saveMessageStart >= SAVE_MESSAGE_MS) {
                saveMessageActive = false;
                display.setSegments(SEG_PLAY, 4, 0);
            }
            break;

        case MODE_SELECT_POT:
            selectedPot =
                editValue(selectedPot + 1, 1, NUM_POTS, CHAR_POT) - 1;
            break;

        case MODE_SET_CC:
            config[selectedPot].cc =
                editValue(config[selectedPot].cc, 0, CC_MAX, CHAR_CC);
            break;

        case MODE_SET_LOWER:
            config[selectedPot].lower =
                editValue(config[selectedPot].lower, 0, VALUE_MAX, CHAR_LOWER);
            break;

        case MODE_SET_UPPER:
            config[selectedPot].upper =
                editValue(config[selectedPot].upper, 0, VALUE_MAX, CHAR_UPPER);
            break;

        default:
            break;
    }
}
