// /*********************************************************************
//  Adafruit invests time and resources providing this open source code,
//  please support Adafruit and open-source hardware by purchasing
//  products from Adafruit!

//  MIT license, check LICENSE for more information
//  Copyright (c) 2019 Ha Thach for Adafruit Industries
//  All text above, and the splash screen below must be included in
//  any redistribution
// *********************************************************************/

// /* This sketch is enumerated as USB MIDI device.
//  * Following library is required
//  * - MIDI Library by Forty Seven Effects
//  *   https://github.com/FortySevenEffects/arduino_midi_library
//  */

// #include <Arduino.h>
// #include <Adafruit_TinyUSB.h>
// #include <MIDI.h>

// // USB MIDI object
// Adafruit_USBD_MIDI usb_midi;

// // Create a new instance of the Arduino MIDI Library,
// // and attach usb_midi as the transport.
// MIDI_CREATE_INSTANCE(Adafruit_USBD_MIDI, usb_midi, MIDI);

// // Variable that holds the current position in the sequence.
// uint32_t position = 0;

// // LED pin - use the built-in LED pin for your board
// // For most Arduino boards, this is pin 13
// #ifndef LED_BUILTIN
// #define LED_BUILTIN 13
// #endif

// // Store example melody as an array of note values
// byte note_sequence[] = {
//     74, 78, 81, 86, 90, 93, 98, 102, 57, 61, 66, 69, 73, 78, 81, 85, 88, 92, 97, 100, 97, 92, 88, 85, 81, 78,
//     74, 69, 66, 62, 57, 62, 66, 69, 74, 78, 81, 86, 90, 93, 97, 102, 97, 93, 90, 85, 81, 78, 73, 68, 64, 61,
//     56, 61, 64, 68, 74, 78, 81, 86, 90, 93, 98, 102};

// void handleNoteOn(byte channel, byte pitch, byte velocity)
// {
//   // Log when a note is pressed.
//   Serial.print("Note on: channel = ");
//   Serial.print(channel);

//   Serial.print(" pitch = ");
//   Serial.print(pitch);

//   Serial.print(" velocity = ");
//   Serial.println(velocity);
// }

// void handleNoteOff(byte channel, byte pitch, byte velocity)
// {
//   // Log when a note is released.
//   Serial.print("Note off: channel = ");
//   Serial.print(channel);

//   Serial.print(" pitch = ");
//   Serial.print(pitch);

//   Serial.print(" velocity = ");
//   Serial.println(velocity);
// }

// void setup()
// {
//   // Configure LED pin as output
//   pinMode(LED_BUILTIN, OUTPUT);

//   // Manual begin() is required on core without built-in support e.g. mbed rp2040
//   if (!TinyUSBDevice.isInitialized())
//   {
//     TinyUSBDevice.begin(0);
//   }

//   Serial.begin(115200);

//   usb_midi.setStringDescriptor("TinyUSB MIDI");

//   // Initialize MIDI, and listen to all MIDI channels
//   // This will also call usb_midi's begin()
//   MIDI.begin(MIDI_CHANNEL_OMNI);

//   // If already enumerated, additional class driverr begin() e.g msc, hid, midi won't take effect until re-enumeration
//   if (TinyUSBDevice.mounted())
//   {
//     TinyUSBDevice.detach();
//     delay(10);
//     TinyUSBDevice.attach();
//   }

//   // Attach the handleNoteOn function to the MIDI Library. It will
//   // be called whenever the Bluefruit receives MIDI Note On messages.
//   MIDI.setHandleNoteOn(handleNoteOn);

//   // Do the same for MIDI Note Off messages.
//   MIDI.setHandleNoteOff(handleNoteOff);

//   // Blink LED rapidly at startup to indicate the program is running
//   for (int i = 0; i < 6; i++)
//   {
//     digitalWrite(LED_BUILTIN, HIGH);
//     delay(100);
//     digitalWrite(LED_BUILTIN, LOW);
//     delay(100);
//   }
// }

// void loop()
// {
// #ifdef TINYUSB_NEED_POLLING_TASK
//   // Manual call tud_task since it isn't called by Core's background
//   TinyUSBDevice.task();
// #endif

//   // not enumerated()/mounted() yet: nothing to do
//   if (!TinyUSBDevice.mounted())
//   {
//     // Blink LED slowly when not mounted
//     static uint32_t blink_ms = 0;
//     if (millis() - blink_ms > 500)
//     {
//       blink_ms = millis();
//       digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
//     }
//     return;
//   }

//   static uint32_t start_ms = 0;
//   if (millis() - start_ms > 266)
//   {
//     start_ms += 266;

//     // Blink LED with each note
//     digitalWrite(LED_BUILTIN, HIGH);

//     // Setup variables for the current and previous
//     // positions in the note sequence.
//     int previous = position - 1;

//     // If we currently are at position 0, set the
//     // previous position to the last note in the sequence.
//     if (previous < 0)
//     {
//       previous = sizeof(note_sequence) - 1;
//     }

//     // Send Note On for current position at full velocity (127) on channel 1.
//     MIDI.sendNoteOn(note_sequence[position], 127, 1);

//     // Send Note Off for previous note.
//     MIDI.sendNoteOff(note_sequence[previous], 0, 1);

//     // Increment position
//     position++;

//     // If we are at the end of the sequence, start over.
//     if (position >= sizeof(note_sequence))
//     {
//       position = 0;
//     }

//     // Turn LED off shortly after each note
//     delay(50);
//     digitalWrite(LED_BUILTIN, LOW);
//   }

//   // read any new MIDI messages
//   MIDI.read();
// }