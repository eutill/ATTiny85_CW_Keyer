/*!
 
 @file      yack.c
 @brief     CW Keyer library
 @author    Jan Lategahn DK3LJ jan@lategahn.com (C) 2011; modified by Jack Welch AI4SV; modified by Don Froula WD9DMP
 
 @version   0.87
 
 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 
 @date      15.10.2010  - Created
 @date      03.10.2013  - Last update
 @date      21.12.2016  - Added additional prosigns and punctuation. Added 2 additional memories for ATTINY85. Fixed save of speed change to EEPROM. (WD9DMP)
 @date      03.01.2017  - If memory recording is interrupted by command button, keyer now returns txok ("R") and stays in command mode. Memory is unchanged.
 Memory playback halts immediately on command key instead of looping through message length without playing anything.
 Removed playback of recorded message before saving.
 Changed yackstring command to return to command mode instead of normal mode if interrupted with command key
 
 @todo      Make the delay dependent on T/C 1 

 */

#include <avr/io.h> 
#include <avr/pgmspace.h>
#include <avr/eeprom.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <util/delay.h>
#include <stdint.h>
#include "yack.h"

// Forward declaration of private functions
static void key(byte mode);
static char morsechar(byte buffer);
static void keylatch(void);

// Enumerations

typedef enum {
	STATE_IDLE,
	STATE_DAHDIT_INIT,
	STATE_DAHDIT,
	STATE_IEG,
	STATE_ICG,
	STATE_IWG
} iambicState_t;

typedef enum {
	SYMBOL_NONE,
	SYMBOL_DIT,
	SYMBOL_DAH,
	SYMBOL_OPPOSITE
} symbol_t;

typedef enum {
	PRESSED_NONE,
	PRESSED_ONE,
	PRESSED_BOTH,
	PRESSED_DONT_CARE
} pressedKeys_t;

// Module local definitions

static byte yackflags;		// Permanent (stored) status of module flags
static byte volflags = 0;		// Temporary working flags (volatile)
static word ctcvalue;		// Pitch
static word wpmcnt;			// Speed
static byte wpm;            // Real wpm
static byte farnsworth;     // Additional Farnsworth pause

// EEPROM Data

byte magic EEMEM = MAGPAT;	// Needs to contain 'A5' if mem is valid
byte flagstor EEMEM = FLAGDEFAULT;	//	Defaults
word ctcstor EEMEM = DEFCTC;	// Pitch = 800Hz
byte wpmstor EEMEM = DEFWPM;	// 15 WPM
byte fwstor EEMEM = 0; // No farnsworth pause
word user1 EEMEM = 0; // User storage
word user2 EEMEM = 0; // User storage

char eebuffer1[100] EEMEM = "message 1";
char eebuffer2[100] EEMEM = "message 2";

// Flash data

//! Morse code table in Flash

//! Encoding: Each byte is read from the left. 0 stands for a dot, 1
//! stands for a dash. After each played element the content is shifted
//! left. Playback stops when the leftmost bit contains a "1" and the rest
//! of the bits are all zero.
//!
//! Example: A = .-
//! Encoding: 01100000
//!           .-
//!             | This is the stop marker (1 with all trailing zeros)

const byte morse[] PROGMEM =
{

	0b11111100, // 0
	0b01111100,// 1
	0b00111100,// 2
	0b00011100,// 3
	0b00001100,// 4
	0b00000100,// 5
	0b10000100,// 6
	0b11000100,// 7
	0b11100100,// 8
	0b11110100,// 9
	0b01100000,// A
	0b10001000,// B
	0b10101000,// C
	0b10010000,// D
	0b01000000,// E
	0b00101000,// F
	0b11010000,// G
	0b00001000,// H
	0b00100000,// I
	0b01111000,// J
	0b10110000,// K
	0b01001000,// L
	0b11100000,// M
	0b10100000,// N
	0b11110000,// O
	0b01101000,// P
	0b11011000,// Q
	0b01010000,// R
	0b00010000,// S
	0b11000000,// T
	0b00110000,// U
	0b00011000,// V
	0b01110000,// W
	0b10011000,// X
	0b10111000,// Y
	0b11001000,// Z
	0b00110010,// ?
	0b01010110,// .
	0b10010100,// /
	0b11101000,// ! (American Morse version, commonly used in ham circles)
	0b11001110,// ,
	0b11100010,// :
	0b10101010,// ;
	0b01001010,// "
	0b00010011,// $
	0b01111010,// ' (Apostrophe)
	0b10110100,// ( or [ (also prosign KN)
	0b10110110,// ) or ]
	0b10000110,// - (Hyphen or single dash)
	0b01101010,// @
	0b00110110,// _ (Underline)
	0b01010010,// Paragaraph break symbol
	0b10001100,// = and BT
	0b00010110,// SK
	0b01010100,// + and AR
	0b10001011,// BK
	0b01000100,// AS
	0b10101100,// KA (also ! in alternate Continental Morse)
	0b00010100,// VE
	0b01011000// AA
};

// The special characters at the end of the above table can not be decoded
// without a small table to define their content. # stands for SK, $ for AR

// To add new characters, add them in the code table above at the end and below
// Do not forget to increase the legth of the array..

const char spechar[24] PROGMEM = "?./!,:;~$^()-@_|=#+*%&<>";

// Functions

// ***************************************************************************
// Control functions
// ***************************************************************************

void yackreset(void)
/*! 
 @brief     Sets all yack parameters to standard values

 This function resets all YACK EEPROM settings to their default values as 
 stored in the .h file. It sets the dirty flag and calls the save routine
 to write the data into EEPROM immediately.
 */
{

	ctcvalue = DEFCTC; // Initialize to 800 Hz
	wpm = DEFWPM; // Init to default speed
	wpmcnt = (1200 / YACKBEAT) / DEFWPM; // default speed
	farnsworth = 0; // No Farnsworth gap
	yackflags = FLAGDEFAULT;

	volflags |= DIRTYFLAG;
	yacksave(); // Store them in EEPROM

}

void yackinit(void)
/*! 
 @brief     Initializes the YACK library
 
 This function initializes the keyer hardware according to configurations in the .h file.
 Then it attempts to read saved configuration settings from EEPROM. If not possible, it
 will reset all values to their defaults.
 This function must be called once before the remaining fuctions can be used.
 */
{

	byte magval;

	// Configure DDR. Make OUT and ST output ports
	SETBIT(OUTDDR, OUTPIN);
	SETBIT(STDDR, STPIN);

	// Raise internal pullups for all inputs
	SETBIT(KEYPORT, DITPIN);
	SETBIT(KEYPORT, DAHPIN);
	SETBIT(BTNPORT, BTNPIN);

	magval = eeprom_read_byte(&magic); // Retrieve magic value

	if (magval == MAGPAT) // Is memory valid
	{
		ctcvalue = eeprom_read_word(&ctcstor); // Retrieve last ctc setting
		wpm = eeprom_read_byte(&wpmstor); // Retrieve last wpm setting
		wpmcnt = (1200 / YACKBEAT) / wpm; // Calculate speed
		farnsworth = eeprom_read_byte(&fwstor); // Retrieve last wpm setting	
		yackflags = eeprom_read_byte(&flagstor); // Retrieve last flags	
	} else {
		yackreset();
	}

	yackinhibit(OFF);

#ifdef POWERSAVE
#ifdef TINY85
	PCMSK |= PWRWAKE;    // Define which keys wake us up
	GIMSK |= (1 << PCIE);  // Enable pin change interrupt
#elif defined TINY84
	PCMSK0 |= PWRWAKE;    // Define which keys wake us up
	GIMSK |= (1 << PCIE0);  // Enable pin change interrupt
#endif
#endif

	// Initialize timer1 to serve as the system heartbeat
	// CK runs at 1MHz. Prescaling by 64 makes that 15625 Hz.
	// Counting 78 cycles of that generates an overflow every 5ms

#ifdef TINY85
	OCR1C = 78; // 77 counts per cycle
	TCCR1 |= (1 << CTC1) | 0b00000111; // Clear Timer on match, prescale ck by 64
	OCR1A = 1; // CTC mode does not create an overflow so we use OCR1A
#elif defined TINY84
	OCR1AL = 78; // 77 counts per cycle
	TCCR1B |= (1 << WGM12) | 0b00000011; // Clear Timer on match, prescale ck by 64
#endif
}

#ifdef POWERSAVE

ISR( PCINT0_vect)
/*! 
 @brief     A dummy pin change interrupt
 
 This function is called whenever the system is in sleep mode and there is a level change on one of the contacts 
 we are monitoring (dit, dah and the command key). As all handling is already taken care of by polling in the main 
 routines, there is nothing we need to do here.
 */
{
	// Nothing to do here. All we want is to wake up..
}

void yackpower(byte n)
/*! 
 @brief     Manages the power saving mode
 
 This is called in yackbeat intervals with either a TRUE or FALSE as parameter. Whenever the
 parameter is TRUE a beat counter is advanced until the timeout level is reached. When timeout
 is reached, the chip shuts down and will only wake up again when issued a level change interrupt on
 either of the input pins.
 
 When the parameter is FALSE, the counter is reset.
 
 @param n   TRUE: OK to sleep, FALSE: Can not sleep now
 
 */

{
	static uint32_t shdntimer = 0;

	if (n) // True = we could go to sleep
	{
		if (shdntimer++ == YACKSECS(PSTIME)) {
			shdntimer = 0; // So we do not go to sleep right after waking up..

#ifdef TINY85
			GIFR |= (1 << PCIF); //Clear interrupt flag
#elif defined TINY84
			GIFR |= (1 << PCIF0); //Clear interrupt flag
#endif

      set_sleep_mode(SLEEP_MODE_PWR_DOWN);
			sleep_enable();
			sleep_bod_disable();
			sei();
			sleep_cpu();
			sleep_disable();
			cli();

			// There is no technical reason to CLI here but it avoids hitting the ISR every time
			// the paddles are touched. If the remaining code needs the interrupts this is OK to remove.

		}

	} else // Passed parameter is FALSE
	{
		shdntimer = 0;
	}

}

#endif

void yacksave(void)
/*! 
 @brief     Saves all permanent settings to EEPROM
 
 To save EEPROM write cycles, writing only happens when the flag DIRTYFLAG is set.
 After writing the flag is cleared
 
 @callergraph
 
 */
{

	if (volflags & DIRTYFLAG) // Dirty flag set?
	{

		eeprom_write_byte(&magic, MAGPAT);
		eeprom_write_word(&ctcstor, ctcvalue);
		eeprom_write_byte(&wpmstor, wpm);
		eeprom_write_byte(&flagstor, yackflags);
		eeprom_write_byte(&fwstor, farnsworth);

		volflags &= ~DIRTYFLAG; // Clear the dirty flag
	}

}

void yackinhibit(byte mode)
/*! 
 @brief     Inhibits keying during command phases
 
 This function is used to inhibit and re-enable TX keying (if configured) and enforce the internal 
 sidetone oscillator to be active so that the user can communicate with the keyer.
 
 @param mode   ON inhibits keying, OFF re-enables keying 
 
 */
{

	if (mode) {
		volflags &= ~(TXKEY | SIDETONE);
		volflags |= SIDETONE;
	}

	else

	{
		volflags &= ~(TXKEY | SIDETONE);
		volflags |= (yackflags & (TXKEY | SIDETONE));
		key(UP);
	}

}

word yackuser(byte func, byte nr, word content)
/*! 
 @brief     Saves user defined settings
 
 The routine using this library is given the opportunity to save up to two 16 bit sized
 values in EEPROM. In case of the sample main function this is used to store the beacon interval 
 timer value. The routine is not otherwise used by the library.
 
 @param func    States if the data is retrieved (READ) or written (WRITE) to EEPROM
 @param nr      1 or 2 (Number of user storage to access)
 @param content The 16 bit word to write. Not used in read mode.
 @return        The content of the retrieved value in read mode.
 
 */
{

	if (func == READ) {
		if (nr == 1)
			return (eeprom_read_word(&user1));
		else if (nr == 2)
			return (eeprom_read_word(&user2));
	}

	if (func == WRITE) {

		if (nr == 1)
			eeprom_write_word(&user1, content);
		else if (nr == 2)
			eeprom_write_word(&user2, content);
	}

	return (FALSE);

}

word yackwpm(void)
/*! 
 @brief     Retrieves the current WPM speed
 
 This function delivers the current WPM speed. 

 @return        Current speed in WPM
 
 */
{

	return wpm;

}

void yackspeed(byte dir, byte mode)
/*! 
 @brief     Increases or decreases the current WPM speed
 
 The amount of increase or decrease is in amounts of wpmcnt. Those are close to real
 WPM in a 10ms heartbeat but can significantly differ at higher heartbeat speeds.
 
 @param dir     UP (faster) or DOWN (slower)
 
 */
{

	if (mode == FARNSWORTH) {
		if ((dir == UP) && (farnsworth > 0))
			farnsworth--;

		if ((dir == DOWN) && (farnsworth < MAXFARN))
			farnsworth++;
	} else // WPMSPEED
	{
		if ((dir == UP) && (wpm < MAXWPM))
			wpm++;

		if ((dir == DOWN) && (wpm > MINWPM))
			wpm--;

		wpmcnt = (1200 / YACKBEAT) / wpm; // Calculate beats

	}

	volflags |= DIRTYFLAG; // Set the dirty flag

	yackplay(DIT);
	yackdelay(IEGLEN);	// Inter Element gap
	yackplay(DAH);
	yackdelay(ICGLEN);	// Inter Character gap
	yackfarns(); // Additional Farnsworth delay

}

void yackbeat(void)
/*! 
 @brief     Heartbeat delay
 
 Several functions in the keyer are timing dependent. The most prominent example is the
 yackiambic function that implements the IAMBIC keyer finite state machine.
 The same expects to be called in intervals of YACKBEAT milliseconds. How this is 
 implemented is left to the user. In a more complex application this would be done
 using an interrupt or a timer. For simpler cases this is a busy wait routine
 that delays exactly YACKBEAT ms.
 
 */
{
#ifdef TINY85
	while ((TIFR & (1 << OCF1A)) == 0)
			; // Wait for Timeout
		TIFR |= (1 << OCF1A);                // Reset output compare flag
#elif defined TINY84
	while ((TIFR1 & (1 << OCF1A)) == 0)
		; // Wait for Timeout
	TIFR1 |= (1 << OCF1A);                // Reset output compare flag
#endif
}

void yackpitch(byte dir)
/*! 
 @brief     Increases or decreases the sidetone pitch
 
 Changes are done not in Hz but in ctc control values. This is to avoid extensive 
 calculations at runtime. As is all calculations are done by the preprocessor.
 
 @param dir     UP or DOWN
 
 */
{
	if (dir == UP)
		ctcvalue--;
	if (dir == DOWN)
		ctcvalue++;

	if (ctcvalue < MAXCTC)
		ctcvalue = MAXCTC;

	if (ctcvalue > MINCTC)
		ctcvalue = MINCTC;

	volflags |= DIRTYFLAG; // Set the dirty flag

}

void yacktune(void)
/*! 
 @brief     Activates Tuning mode
 
 This produces a solid keydown for TUNEDURATION seconds. After this the TX is unkeyed.
 The same can be achieved by presing either the DIT or the DAH contact or the control key.
 
 */
{
	word timer = YACKSECS(TUNEDURATION);

	key(DOWN);

	while (timer && (KEYINP & (1 << DITPIN)) && (KEYINP & (1 << DAHPIN))
			&& !yackctrlkey(TRUE)) {
		timer--;
		yackbeat();
	}

	key(UP);
}

void yackmode(byte mode)
/*! 
 @brief     Sets the keyer mode (e.g. IAMBIC A)
 
 This allows to set the content of the two mode bits in yackflags. Currently only
 two modes are supported, IAMBIC A and IAMBIC B.
 
 @param mode    IAMBICA or IAMBICB
 @return    TRUE is all was OK, FALSE if configuration lock prevented changes
 
 */
{

	yackflags &= ~MODE;
	yackflags |= mode;

	volflags |= DIRTYFLAG; // Set the dirty flag	

}

byte yackflag(byte flag)
/*! 
 @brief     Query feature flags
 
 @param flag A byte which indicate which flags are to be queried 
 @return     0 if the flag(s) were clear, >0 if flag(s) were set
 
 */
{
	return yackflags & flag;
}

void yacktoggle(byte flag)
/*! 
 @brief     Toggle feature flags
 
 When passed one (or more) flags, this routine flips the according bit in yackflags and
 thereby enables or disables the corresponding feature.
 
 @param flag    A byte where any bit to toggle is set e.g. SIDETONE 
 @return    TRUE if all was OK, FALSE if configuration lock prevented changes
 
 */
{

	yackflags ^= flag;      // Toggle the feature bit
	volflags |= DIRTYFLAG;  // Set the dirty flag

}

void yackerror(void)
/*! 
 @brief     Creates a series of 8 dits
 
 The error prosign (8 dits) can not be encoded in our coding table. A call to this
 function produces it..
 
 */
{
	byte i;

	for (i = 0; i < 8; i++) {
		yackplay(DIT);
		yackdelay(DITLEN);
	}
	yackdelay(DAHLEN);

}

// ***************************************************************************
// CW Playback related functions
// ***************************************************************************

static void key(byte mode)
/*! 
 @brief     Keys the transmitter and produces a sidetone
 
 .. but only if the corresponding functions (TXKEY and SIDETONE) have been set in
 the feature register. This function also handles a request to invert the keyer line
 if necessary (TXINV bit).
 
 This is a private function.

 @param mode    UP or DOWN
 
 */
{

	if (mode == DOWN) {
		if (volflags & SIDETONE) // Are we generating a Sidetone?
		{
			OCR0A = ctcvalue;		// Then switch on the Sidetone generator
#ifdef TINY85
			OCR0B = ctcvalue;

			// Activate CTC mode
			TCCR0A |= (1 << COM0B0 | 1 << WGM01);
#elif defined TINY84
			// Activate CTC mode
			TCCR0A |= (1 << COM0A0 | 1 << WGM01);
#endif


			// Configure prescaler
			TCCR0B = 1 << CS01;
		}

		if (volflags & TXKEY) // Are we keying the TX?
		{
			if (yackflags & TXINV) // Do we need to invert keying?
				CLEARBIT(OUTPORT, OUTPIN);
			else
				SETBIT(OUTPORT, OUTPIN);
		}

	}

	if (mode == UP) {

		if (volflags & SIDETONE) // Sidetone active?
		{
			TCCR0A = 0;
			TCCR0B = 0;
		}

		if (volflags & TXKEY) // Are we keying the TX?
		{
			if (yackflags & TXINV) // Do we need to invert keying?
				SETBIT(OUTPORT, OUTPIN);
			else
				CLEARBIT(OUTPORT, OUTPIN);
		}

	}

}

void yackfarns(void)
/*! 
 @brief     Produces an additional waiting delay for farnsworth mode.
 
 */
{

	word i = farnsworth;

	while (i--) {
		yackdelay(1);
	}

}

void yackdelay(byte n)
/*! 
 @brief     Produces an active waiting delay for n dot counts
 
 This is used during the playback functions where active waiting is needed
 
 @param n   number of dot durations to delay (dependent on current keying speed!
 
 */
{

	byte i = n;
	byte x;

	while (i--) {
		x = wpmcnt;
		while (x--)
			yackbeat();
	}

}

void yackplay(byte i)
/*! 
 @brief     Key the TX / Sidetone for the duration of a dit or a dah
 
 @param i   DIT or DAH
 
 */
{

	key(DOWN);

#ifdef POWERSAVE

	yackpower(FALSE); // Avoid powerdowns when keying

#endif

	switch (i) {
	case DAH:
		yackdelay(DAHLEN);
		break;

	case DIT:
		yackdelay(DITLEN);
		break;
	}

	key(UP);

}

void yackchar(char c)
/*! 
 @brief     Send a character in morse code
 
 This function translates a character passed as parameter into morse code using the 
 translation table in Flash memory. It then keys transmitter / sidetone with the characters
 elements and adds all necessary gaps (as if the character was part of a longer word).
 
 If the character can not be translated, nothing is sent.
 
 If a space is received, an interword gap is sent.

 @param c   The character to send
 
 */

{
	byte code = 0x80; // 0x80 is an empty morse character (just eoc bit set)
	byte i; // a counter

	// First we need to map the actual character to the encoded morse sequence in
	// the array "morse"
	if (c >= '0' && c <= '9') // Is it a numerical digit?
		code = pgm_read_byte(&morse[c - '0']); // Find it in the beginning of array

	if (c >= 'a' && c <= 'z') // Is it a character?
		code = pgm_read_byte(&morse[c - 'a' + 10]); // Find it from position 10

	if (c >= 'A' && c <= 'Z') // Is it a character in upper case?
		code = pgm_read_byte(&morse[c - 'A' + 10]); // Same as above

	// Last we need to handle special characters. There is a small char
	// array "spechar" which contains the characters for the morse elements
	// at the end of the "morse" array (see there!)
	for (i = 0; i < sizeof(spechar); i++) // Read through the array
		if (c == pgm_read_byte(&spechar[i])) // Does it contain our character
			code = pgm_read_byte(&morse[i + 36]); // Map it to morse code

	if (c == ' ') // Do they want us to transmit a space (a gap of 7 dots)
		yackdelay(IWGLEN - ICGLEN); // ICG was already played after previous char
	else {
		while (code != 0x80) // Stop when EOC bit has reached MSB
		{
			if (yackctrlkey(FALSE)) // Stop playing if someone pushes key
				return;

			if (code & 0x80) 	// MSB set ?
				yackplay(DAH);      // ..then play a dash
			else
				// MSB cleared ?
				yackplay(DIT);		// .. then play a dot

			yackdelay(IEGLEN);	// Inter Element gap

			code = code << 1;	// Shift code on position left (to next element)
		}

		yackdelay(ICGLEN - IEGLEN); // IEG was already played after element

		yackfarns(); // Insert another gap for farnsworth keying

	}

}

void yackstring(const char *p)
/*! 
 @brief     Sends a 0-terminated string in CW which resides in Flash
 
 Reads character by character from flash, translates into CW and keys the transmitter
 and/or sidetone depending on feature bit settings.
 
 @param p   Pointer to string location in FLASH 
 
 */
{

	char c;

	while ((c = pgm_read_byte(p++)) && !(yackctrlkey(TRUE)))
		// While end of string in flash not reached and ctrl not pressed
		yackchar(c);            // Play the read character
								// abort now if someone presses command key

}

void yacknumber(word n)
/*! 
 @brief     Sends a number in CW
 
 Transforms a number up to 65535 into its digits and sends them in CW
 
 @param n   The number to send
 
 */

{

	char buffer[5];
	byte i = 0;

	while (n) // Until nothing left or control key pressed
	{
		buffer[i++] = n % 10 + '0'; // Store rest of division by 10
		n /= 10;                // Divide by 10
	}

	while (i) {
		if (yackctrlkey(TRUE)) {
			break;
		}
		yackchar(buffer[--i]);
	}

	yackchar(' ');

}

// ***************************************************************************
// CW Keying related functions
// ***************************************************************************

static void keylatch(void)
/*! 
 @brief     Latches the status of the DIT and DAH paddles
 
 If either DIT or DAH are keyed, this function sets the corresponding bit in 
 volflags. This is used by the IAMBIC keyer to determine which element needs to 
 be sounded next.
 
 This is a private function.

 */
{

	byte swap;	 // Status of swap flag

	swap = (yackflags & PDLSWAP);

	if (!( KEYINP & (1 << DITPIN)))
		volflags |= (swap ? DAHLATCH : DITLATCH);

	if (!( KEYINP & (1 << DAHPIN)))
		volflags |= (swap ? DITLATCH : DAHLATCH);

}

byte yackctrlkey(byte mode)
/*! 
 @brief     Scans for the Control key
 
 This function is regularly called at different points in the program. In a normal case
 it terminates instantly. When the command key is found to be closed, the routine idles
 until it is released again and returns a TRUE return value.
 
 If, during the period where the contact was closed one of the paddles was closed too,
 the wpm speed is changed and the keypress not interpreted as a Control request. 

 @param mode    TRUE if caller has taken care of command key press, FALSE if not
 @return        TRUE if a press of the command key is not yet handled. 
 
 @callergraph
 
 */
{

	byte volbfr;

	volbfr = volflags; // Remember current volatile settings

	if (!(BTNINP & (1 << BTNPIN))) // If command button is pressed
	{

		volbfr |= CKLATCH; // Set control key latch

		// Apparently the control key has been pressed. To avoid bouncing
		// We will now wait a short while and then busy wait until the key is
		// released.
		// Should we find that someone is keying the paddle, let him change
		// the speed and pretend ctrl was never pressed in the first place..

		yackinhibit(ON); // Stop keying, switch on sidetone.

		_delay_ms(50);

		while (!(BTNINP & (1 << BTNPIN))) // Busy wait for release
		{

			if (!( KEYINP & (1 << DITPIN))) // Someone pressing DIT paddle
			{
				yackspeed(DOWN, WPMSPEED);
				volbfr &= ~(CKLATCH); // Ignore that control key was pressed
			}

			if (!( KEYINP & (1 << DAHPIN))) // Someone pressing DAH paddle
			{
				yackspeed(UP, WPMSPEED);
				volbfr &= ~(CKLATCH);
			}

		}

		_delay_ms(50); // Trailing edge debounce

		yacksave();	// In case we had a speed change

	}

	volflags = volbfr; // Restore previous state

	if (mode == TRUE) // Does caller want us to reset latch?
	{
		volflags &= ~(CKLATCH);
	}

	//yacksave(); // In case we had a speed change (Does NOT work if command is here - moved immediately after button release debounce)

	return ((volbfr & CKLATCH) != 0); // Tell caller if we had a ctrl button press

}

static char morsechar(byte buffer)
/*! 
 @brief     Reverse maps a combination of dots and dashes to a character
 
 This routine is passed a sequence of dots and dashes in the format we use for morse
 character encoding (see top of this file). It looks up the corresponding character in
 the Flash table and returns it to the caller. 
 
 This is a private function.
 
 @param buffer    A character in YACK CW notation
 @return          The mapped character or /0 if no match was found  
 
 */
{
	byte i;

	for (i = 0; i < sizeof(morse); i++) {

		if (pgm_read_byte(&morse[i]) == buffer) {
			if (i < 10)
				return ('0' + i);		// First 10 chars are digits
			if (i < 36)
				return ('A' + i - 10);	// Then follow letters
			return (pgm_read_byte(&spechar[i - 36])); // Then special chars
		}

	}

	return '\0';
}

void yackmessage(byte function, byte msgnr)
/*! 
 @brief     Handles EEPROM stored CW messages (macros)
 
 When called in RECORD mode, the function records a message up to 100 characters and stores it in 
 EEPROM. The routine stops recording when timing out after DEFTIMEOUT seconds. Recording
 can be aborted using the control key. If more than 100 characters are recorded, the error prosign
 is sounded and recording starts from the beginning. After recording and timing out the message is played
 back once before it is stored. To erase a message, do not key one.
 
 When called in PLAY mode, the message is just played back. Playback can be aborted using the command
 key.
 
 @param     function    RECORD or PLAY
 @param     msgnr       1 or 2
 @return    TRUE if all OK, FALSE if lock prevented message recording
 
 */
{
	unsigned char rambuffer[RBSIZE];	// Storage for the message
	unsigned char c;					// Work character

	word extimer = 0;		// Detects end of message (10 sec)

	byte i = 0;				// Pointer into RAM buffer
	byte n;					// Generic counter

	if (function == RECORD) {

		extimer = YACKSECS(DEFTIMEOUT);	// 5 Second until message end
		while (extimer--)	// Continue until we waited 10 seconds
		{
			if (yackctrlkey(TRUE))
				return;

			if ((c = yackiambic(ON))) // Check for a character from the key
			{
				rambuffer[i++] = c; // Add that character to our buffer
				extimer = YACKSECS(DEFTIMEOUT); // Reset End of message timer
			}

			if (i >= RBSIZE) // End of buffer reached?
			{
				yackerror();
				i = 0;
			}

			yackbeat(); // 10 ms heartbeat
		}

		// Extimer has expired. Message has ended

		if (i) // Was anything received at all?
		{
			rambuffer[--i] = 0; // Add a \0 end marker over last space

			// Replay the message
			//for (n=0;n<i;n++){
			//	if (yackctrlkey(TRUE)) {return;} //Break to command mode without saving if command key pressed
			//	yackchar(rambuffer[n]);
			//}

			// Store it in EEPROM
			if (msgnr == 1)
				eeprom_write_block(rambuffer, eebuffer1, RBSIZE);
			if (msgnr == 2)
				eeprom_write_block(rambuffer, eebuffer2, RBSIZE);
		} else
			yackerror();
	}

	if (function == PLAY) {
		// Retrieve the message from EEPROM
		if (msgnr == 1)
			eeprom_read_block(rambuffer, eebuffer1, RBSIZE);
		if (msgnr == 2)
			eeprom_read_block(rambuffer, eebuffer2, RBSIZE);

		// Replay the message
		for (n = 0; (c = rambuffer[n]); n++) { // Read until end of message
			if (yackctrlkey(FALSE)) {
				return;
			} //Break immediately if command key pressed
			yackchar(c); // play it back 
		}

	}

}

char yackiambic(byte ctrl) {

	static iambicState_t iambicState = STATE_IDLE;
	static word timer = 0;
	static word debounceTimer = 0;
	static symbol_t currentSymbol;
	static symbol_t nextSymbol;
	static pressedKeys_t pressedKeys;
	static byte repeatCycle = 0; //without waiting for next yackbeat
	static byte keystate = 0; //debounced key state
	static byte buffer = 0;
	static byte bufctr = 0;

	char retchar = '\0';

	if (timer) {timer--;}
	if (debounceTimer) {debounceTimer--;}

	if(debounceTimer == 0) { //debounce timer not active, read keylatch
		keylatch();
		if(keystate != (volflags & SQUEEZED)) { //keys changed state
			debounceTimer = KEYDEBOUNCE / YACKBEAT;
			keystate = volflags & SQUEEZED;
		}
		volflags &= ~(DITLATCH | DAHLATCH);
	}


	do {
		repeatCycle = 0;

		switch(iambicState) {

		case STATE_IDLE:
		case STATE_ICG:
		case STATE_IWG:

#ifdef POWERSAVE
			yackpower(TRUE);
#endif
			if(keystate) { //some key has been pressed
				if(keystate == DITLATCH) {
					nextSymbol = SYMBOL_DIT;
				} else if(keystate == DAHLATCH) {
					nextSymbol = SYMBOL_DAH;
				}
				iambicState = STATE_DAHDIT_INIT;
				repeatCycle = 1;
			}

			if((iambicState != STATE_IDLE) && (timer == 0)) {
				if(iambicState == STATE_ICG) {
					//char is done
					buffer = buffer << 1;	  // Make space for the termination bit
					buffer |= 1;			  // The 1 on the right signals end
					buffer = buffer << (7 - bufctr); // Shift to left justify
					retchar = morsechar(buffer); // Attempt decoding
					buffer = bufctr = 0;			// Clear buffer

					timer = (IWGLEN - ICGLEN) * wpmcnt;
					iambicState = STATE_IWG;
				} else if(iambicState == STATE_IWG) {
					//word is done
					if(ctrl == ON) {
						retchar = ' ';
					}
					iambicState = STATE_IDLE;
				}
			}
			break;


		case STATE_DAHDIT_INIT:
#ifdef POWERSAVE
			yackpower(FALSE);
#endif
			currentSymbol = nextSymbol;
			nextSymbol = SYMBOL_NONE;

			buffer = buffer << 1;
			bufctr++;

			switch(currentSymbol) {
			case SYMBOL_DIT:
				timer = DITLEN * wpmcnt;
				break;
			case SYMBOL_DAH:
				timer = DAHLEN * wpmcnt;
				buffer |= 1;
				break;
			default:
				iambicState = STATE_IDLE; //this shouldn't happen
				continue;
				break;
			}

			if(keystate == SQUEEZED) {
				pressedKeys = PRESSED_BOTH;
				if((yackflags & MODE) == IAMBICB) {
					nextSymbol = SYMBOL_OPPOSITE;
				}
			} else if(keystate == DITLATCH) {
				pressedKeys = PRESSED_ONE;
				if(currentSymbol != SYMBOL_DIT) {
					nextSymbol = SYMBOL_DIT;
					pressedKeys = PRESSED_DONT_CARE;
				}
			} else if(keystate == DAHLATCH) {
				pressedKeys = PRESSED_ONE;
				if(currentSymbol != SYMBOL_DAH) {
					nextSymbol = SYMBOL_DAH;
					pressedKeys = PRESSED_DONT_CARE;
				}
			} else {
				pressedKeys = PRESSED_NONE;
			}

			key(DOWN);
			iambicState = STATE_DAHDIT;
			break;


		case STATE_DAHDIT:
		case STATE_IEG:
			switch(pressedKeys) {
			case PRESSED_BOTH:
				if(keystate == 0) { //None pressed
					pressedKeys = PRESSED_NONE;
				} else if(keystate != SQUEEZED) { //One pressed
					pressedKeys = PRESSED_ONE;
				}
				break;
			case PRESSED_ONE:
				if(keystate == SQUEEZED) { //Both pressed
					nextSymbol = SYMBOL_OPPOSITE;
					pressedKeys = PRESSED_DONT_CARE;
				} else if(keystate == 0) { //None pressed
					pressedKeys = PRESSED_NONE;
				}
				break;
			case PRESSED_NONE:
				if(keystate == SQUEEZED) {
					nextSymbol = SYMBOL_OPPOSITE;
					pressedKeys = PRESSED_DONT_CARE;
				} else if(keystate == DITLATCH) {
					nextSymbol = SYMBOL_DIT;
					pressedKeys = PRESSED_DONT_CARE;
				} else if(keystate == DAHLATCH) {
					nextSymbol = SYMBOL_DAH;
					pressedKeys = PRESSED_DONT_CARE;
				}
				break;
			default:
				break;
			}

			if(timer == 0) {
				if(iambicState == STATE_DAHDIT) {
					if(pressedKeys == PRESSED_BOTH) {
						nextSymbol = SYMBOL_OPPOSITE;
						pressedKeys = PRESSED_DONT_CARE;
					}
					key(UP);
					timer = IEGLEN * wpmcnt;
					iambicState = STATE_IEG;
				} else { //STATE_IEG
					if(pressedKeys == PRESSED_ONE) {
						if(keystate == DITLATCH) {
							nextSymbol = SYMBOL_DIT;
						} else if(keystate == DAHLATCH) {
							nextSymbol = SYMBOL_DAH;
						} else { //shouldn't happen
							nextSymbol = SYMBOL_NONE;
						}
					}
					if(nextSymbol == SYMBOL_OPPOSITE) {
						if(currentSymbol == SYMBOL_DIT) {
							nextSymbol = SYMBOL_DAH;
						} else {
							nextSymbol = SYMBOL_DIT;
						}
					}

					if(nextSymbol == SYMBOL_NONE) {
						iambicState = STATE_ICG;
						timer = (ICGLEN - IEGLEN - 1) * wpmcnt;
					} else {
						iambicState = STATE_DAHDIT_INIT;
						repeatCycle = 1;
					}
				}
			}
			break;
		}
	} while (repeatCycle);

	return retchar;
}

