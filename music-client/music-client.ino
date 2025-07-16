#if ARDUINO_USB_MODE
#warning This sketch should be used when USB is in OTG mode
void setup() {}
void loop() {}
#else
#include <math.h>
#include "USB.h"
#include "USBMIDI.h"
USBMIDI MIDI;
#define MIDI_NOTE_C4 60
#define MIDI_CC_CUTOFF 74

// F Minor scale MIDI note numbers from C2 to C5
// F Minor: F, G, Ab, Bb, C, Db, Eb
const int fMinorScale[] = {
	36, 38, 39, 41, 43, 44, 46,  // C2 to Eb2
	48, 50, 51, 53, 55, 56, 58,  // F2 to Eb3
	60, 62, 63, 65, 67, 68, 70,  // F3 to Eb4
	72, 74, 75, 77, 79, 80, 82,  // F4 to Eb5
	84  // C5
};
const int scaleLength = sizeof(fMinorScale) / sizeof(fMinorScale[0]);

///// Photodetector Handling /////
#define PHOTODETECTOR_PIN 5
#define LIGHT_THRESHOLD_PERCENTAGE 20  // Trigger when darkness rises 20% above minimum
#define SMOOTHING_VALUE 10   // Reduced for faster response
static double photoValue = 0;
static bool noteIsPlaying = false;

void updatePhotoValue() {
	photoValue = (photoValue * (SMOOTHING_VALUE - 1) + analogRead(PHOTODETECTOR_PIN)) / SMOOTHING_VALUE;
}

void primePhotoValue() {
	for (int i = 0; i < SMOOTHING_VALUE * 10; i++) {  // Prime longer for faster settling
		updatePhotoValue();
	}
}

// Get velocity based on light level (0-127 for MIDI)
uint8_t getVelocityFromLight() {
	// Auto-calibrate the photodetector range
	static uint16_t maxPhotoValue = 0;
	static uint16_t minPhotoValue = 4095;
	
	uint16_t currentPhoto = round(photoValue);
	
	if (currentPhoto < minPhotoValue) {
		minPhotoValue = currentPhoto;
		Serial.print("New min photo value: ");
		Serial.println(minPhotoValue);
	}
	if (currentPhoto > maxPhotoValue) {
		maxPhotoValue = currentPhoto;
		Serial.print("New max photo value: ");
		Serial.println(maxPhotoValue);
	}
	
	// Map to MIDI velocity range (1-127, avoid 0 velocity)
	if (maxPhotoValue > minPhotoValue) {
		uint8_t velocity = map(currentPhoto, minPhotoValue, maxPhotoValue, 1, 127);
		Serial.print("Photo: ");
		Serial.print(currentPhoto);
		Serial.print(" (range: ");
		Serial.print(minPhotoValue);
		Serial.print("-");
		Serial.print(maxPhotoValue);
		Serial.print(") -> Velocity: ");
		Serial.println(velocity);
		return velocity;
	}
	Serial.println("Using default velocity (64) - no range established");
	return 64; // Default velocity
}

bool isLightAboveThreshold() {
	// Get current calibration values
	static uint16_t maxPhotoValue = 0;
	static uint16_t minPhotoValue = 4095;
	
	uint16_t currentPhoto = round(photoValue);
	
	// Update calibration
	if (currentPhoto < minPhotoValue) {
		minPhotoValue = currentPhoto;
	}
	if (currentPhoto > maxPhotoValue) {
		maxPhotoValue = currentPhoto;
	}
	
	// Calculate dynamic threshold based on percentage rise from min
	uint16_t dynamicThreshold = minPhotoValue + ((maxPhotoValue - minPhotoValue) * LIGHT_THRESHOLD_PERCENTAGE / 100);
	
	// For inverted circuit: HIGH numbers = DARK = trigger notes
	bool aboveThreshold = currentPhoto > dynamicThreshold;
	
	// Debug output every 20th call to avoid spam
	static int debugCounter = 0;
	if (debugCounter++ % 20 == 0) {
		Serial.print("Dark level: ");
		Serial.print(currentPhoto);
		Serial.print(" | Range: ");
		Serial.print(minPhotoValue);
		Serial.print("-");
		Serial.print(maxPhotoValue);
		Serial.print(" | Threshold: ");
		Serial.print(dynamicThreshold);
		Serial.print(" (");
		Serial.print(LIGHT_THRESHOLD_PERCENTAGE);
		Serial.print("% rise) -> ");
		Serial.println(aboveThreshold ? "DARK (trigger)" : "bright");
	}
	
	return aboveThreshold;
}

///// Note Selection Pot /////
#define NOTE_POT_PIN 7
int currentNote = -1; // Track the currently playing note

// Function to get MIDI note based on pot value
int getMidiNoteFromPot() {
	int potValue = analogRead(NOTE_POT_PIN);
	// Map pot value (0-4095) to scale index (0 to scaleLength-1)
	int scaleIndex = map(potValue, 0, 4095, 0, scaleLength - 1);
	int note = fMinorScale[scaleIndex];
	
	// Debug output every 20th call to avoid spam
	static int debugCounter = 0;
	if (debugCounter++ % 20 == 0) {
		Serial.print("Pot value: ");
		Serial.print(potValue);
		Serial.print(" -> Scale index: ");
		Serial.print(scaleIndex);
		Serial.print(" -> MIDI note: ");
		Serial.println(note);
	}
	
	return note;
}

///// Arduino Hooks /////
void setup() {
	Serial.begin(115200);
	delay(1000); // Give serial time to initialize
	
	Serial.println("=================================");
	Serial.println("Photodetector MIDI Controller");
	Serial.println("=================================");
	
	MIDI.begin();
	USB.begin();
	
	analogReadResolution(12);
	Serial.println("ADC resolution set to 12 bits (0-4095)");
	
	Serial.print("Photodetector pin: ");
	Serial.println(PHOTODETECTOR_PIN);
	Serial.print("Note selection pot pin: ");
	Serial.println(NOTE_POT_PIN);
	Serial.print("Dynamic threshold: Trigger when darkness rises ");
	Serial.print(LIGHT_THRESHOLD_PERCENTAGE);
	Serial.println("% above minimum");
	
	Serial.println("Calibrating photodetector...");
	primePhotoValue();
	Serial.print("Initial light reading: ");
	Serial.println(round(photoValue));
	
	Serial.println("Waiting 3 seconds for sensor to fully settle...");
	for (int i = 3; i > 0; i--) {
		delay(1000);
		updatePhotoValue();
		Serial.print("Settling... Current reading: ");
		Serial.println(round(photoValue));
	};
	
	Serial.println("=================================");
	Serial.println("Ready! Cover photodetector (make it dark) to trigger notes");
	Serial.println("Turn pot to change notes in F minor scale");
	Serial.println("=================================");
}

void loop() {
	updatePhotoValue();
	
	// Check light level 
	bool lightDetected = isLightAboveThreshold();
	
	// Note triggering logic based on photodetector
	if (lightDetected && !noteIsPlaying) {
		// Darkness detected and no note is currently playing - start note
		Serial.println("*** DARKNESS DETECTED - STARTING NOTE ***");
		currentNote = getMidiNoteFromPot();
		uint8_t velocity = getVelocityFromLight();
		
		MIDI.noteOn(currentNote, velocity);
		noteIsPlaying = true;
		
		Serial.print("🎵 Note ON: ");
		Serial.print(currentNote);
		Serial.print(" (Velocity: ");
		Serial.print(velocity);
		Serial.println(")");
		
	} else if (!lightDetected && noteIsPlaying) {
		// Light returned and a note is playing - stop note
		Serial.println("*** LIGHT RETURNED - STOPPING NOTE ***");
		if (currentNote >= 0) {
			MIDI.noteOff(currentNote, 0);
			Serial.print("🎵 Note OFF: ");
			Serial.println(currentNote);
		}
		noteIsPlaying = false;
		currentNote = -1;
		
	} else if (lightDetected && noteIsPlaying) {
		// Still dark and note is playing - update velocity if needed
		static int expressionCounter = 0;
		if (expressionCounter++ % 5 == 0) {  // Update expression less frequently
			uint8_t newVelocity = getVelocityFromLight();
			// Send continuous control for expression
			MIDI.controlChange(11, newVelocity); // CC 11 is Expression controller
		}
		
		// Check if pot position changed significantly
		int newNote = getMidiNoteFromPot();
		if (newNote != currentNote) {
			// Stop current note and start new one
			Serial.println("*** NOTE CHANGE WHILE PLAYING ***");
			MIDI.noteOff(currentNote, 0);
			currentNote = newNote;
			uint8_t velocity = getVelocityFromLight();
			MIDI.noteOn(currentNote, velocity);
			
			Serial.print("🎵 Note change to: ");
			Serial.print(currentNote);
			Serial.print(" (Velocity: ");
			Serial.print(velocity);
			Serial.println(")");
		}
	}
	
	// Status indicator every 100 loops when idle
	static int statusCounter = 0;
	if (!noteIsPlaying && statusCounter++ % 100 == 0) {
		Serial.print("💡 Waiting for darkness... Current: ");
		Serial.print(round(photoValue));
		Serial.println(" (cover sensor to trigger)");
	}
	
	delay(10); // Small delay for stability
}
#endif /* ARDUINO_USB_MODE */