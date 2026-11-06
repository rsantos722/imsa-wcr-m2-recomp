// m2_cport_bits.h — raw M2 ControlPort / StickEventData button bits.
//
// These are the bits the HOST puts into the guest's raw control-port words.
// They are not the guest's action vocabulary: the guest's lifted translators
// (`Cpad_TranslateInput@0x00fe8c`, `Joy_TranslateInput@0x01e438`) index a
// shipped .data table by raw bit position and OR the table entry into the
// translated action word. So the contract this header encodes is exactly
// "which raw bit position does the guest's translator test", and nothing more.
//
// Ownership: the map tables are GUEST data, shipped in the disc image and
// passed by guest code (`StateDriver@0x02fa4c` hard-codes the pair below into
// its stack input record at +0x20/+0x24). The host must not author them.
// See the guest core structure notes ("Control-port raw bits and the
// shipped map tables").
#pragma once

#include <stdint.h>

// --- raw 3DO ControlPort word (digital pad, `Ctrl_TranslateInput` mode 0) ---
// Bit order is the conventional 3DO ControlPort layout and matches the lifted
// translator's test order: `Cpad_TranslateInput` tests raw bits 31..18, so
// every bit below is consumed.
#define M2_CPORT_DOWN   0x80000000u  // raw bit 31 -> table index 0
#define M2_CPORT_UP     0x40000000u  // raw bit 30 -> table index 1
#define M2_CPORT_RIGHT  0x20000000u  // raw bit 29 -> table index 2
#define M2_CPORT_LEFT   0x10000000u  // raw bit 28 -> table index 3
#define M2_CPORT_A      0x08000000u  // raw bit 27 -> table index 4
#define M2_CPORT_B      0x04000000u  // raw bit 26 -> table index 5
#define M2_CPORT_C      0x02000000u  // raw bit 25 -> table index 6
#define M2_CPORT_START  0x01000000u  // raw bit 24 -> table index 10
#define M2_CPORT_X      0x00800000u  // raw bit 23 -> table index 11
#define M2_CPORT_R      0x00400000u  // raw bit 22 -> table index 12
#define M2_CPORT_L      0x00200000u  // raw bit 21 -> table index 13
// M2 extended ControlPad buttons, named by the M2 control-pad interface.
#define M2_CPORT_D      0x00100000u  // table index 7
#define M2_CPORT_E      0x00080000u  // table index 8
#define M2_CPORT_F      0x00040000u  // table index 9

// --- raw 3DO StickEventData button bits (analog joystick) --------------------
// misc/event.h StickEventData: the guest's Joy_TranslateInput tests these
// left-justified bits. The host publishes one generic Stick alongside the pad
// so AssignInputs selects the guest's own mode-7 analog path.
#define M2_STICK_FIRE        0x80000000u
#define M2_STICK_A           0x40000000u
#define M2_STICK_B           0x20000000u
#define M2_STICK_C           0x10000000u
#define M2_STICK_UP          0x08000000u
#define M2_STICK_DOWN        0x04000000u
#define M2_STICK_RIGHT       0x02000000u
#define M2_STICK_LEFT        0x01000000u
#define M2_STICK_PLAY        0x00800000u
#define M2_STICK_STOP        0x00400000u
#define M2_STICK_LEFTSHIFT   0x00200000u
#define M2_STICK_RIGHTSHIFT  0x00100000u

// --- guest table EAs written by guest code (never by the host) --------------
// `StateDriver@0x02fa4c` stores these two pointers into its input record's
// +0x20 (digital/Cpad) and +0x24 (analog/Joy) slots. Both are populated in the
// shipped .data image.
#define M2_CPORT_TABLE_CPAD 0x0017c718u
#define M2_CPORT_TABLE_JOY  0x0017c750u

// --- host-side CportBits reserved for explicit D-pad state ------------------
// These are HOST bits (the `input_map.h` CportBits space), not raw 3DO bits.
// Only explicit button state supplies D-pad bits. Analog samples must never
// infer directions, including when no explicit D-pad button is held.
#define M2_HOST_DPAD_UP     (1u << 8)
#define M2_HOST_DPAD_DOWN   (1u << 9)
#define M2_HOST_DPAD_LEFT   (1u << 10)
#define M2_HOST_DPAD_RIGHT  (1u << 11)
