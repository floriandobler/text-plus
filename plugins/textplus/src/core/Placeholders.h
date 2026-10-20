#pragma once
// Placeholders in any text: live values that are filled in every frame.
//
//   {time} {date} {weekday} {day} {month} {year}    local clock
//   {slide} {slides}                                 slide number and count
//   {bpm}                                            the host's tempo
//   {osc:/address}                                   last OSC value at the address
// ({timer} belongs to the Clock & Timer source, see Clock.h.)
// Unknown placeholders and colour tags stay as they are; "{{" is kept for the
// markup parser, which turns it into a literal "{".

#include "Clock.h"

#include <string>

namespace textplus
{

class OscReceiver;

struct PlaceholderContext
{
	LocalTime now;
	int slide = 1, slides = 1;
	double bpm = 0;
	const OscReceiver* osc = nullptr;
};

// True if `text` contains one of the placeholders above (or {timer}).
bool hasPlaceholders( const std::string& text );
std::string expandPlaceholders( const std::string& text, const PlaceholderContext& ctx );

}//namespace textplus
