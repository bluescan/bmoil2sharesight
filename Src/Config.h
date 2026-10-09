// Config.h
//
// Configuration: loads and saves the BmoIL2ShareSight bmoil2sharesight.cfg file, stored in the tScript s-expr
// notation (a human-readable symbolic expression). The file lives next to the executable.
//
// The config holds:
//   Markets   : a list of [TICKER:CURR MIC] entries (MIC is an ISO 10383 code, which BmoIL2ShareSight maps to the
//               ShareSight Market Code).
//   feeThreshold      : maximum fee (in trade currency) before a group must absorb more legs.
//   maxLookaheadDays  : maximum day span for a group before it must close.
//   Ignored   : tickers dropped from the output (eg. a Norbert's Gambit vehicle).
//   Reviewed  : tickers flagged as a possible Norbert's Gambit and kept, so the prompt is not asked again.
//
// Copyright (c) 2026 Tristan Grimmer.
// Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby
// granted, provided that the above copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
// AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#pragma once
#include <Foundation/tString.h>
#include <Foundation/tList.h>


namespace BMO2SS
{
	// A single market entry: the ticker, its trade currency, and the ISO 10383 market identifier code.
	// Written to the config as [TICKER:CURR MIC].
	struct MarketEntry : tLink<MarketEntry>
	{
		tString Ticker;
		tString Currency;
		tString Mic;
	};


	// Loads and saves the bmoil2sharesight.cfg file. A default-constructed object has every value at its default, so
	// Load() can be called on a fresh instance and any key the file does not record keeps that default.
	class Configuration
	{
	public:
		Configuration();

		// Reads the config file into the members. A key that is absent keeps its current value, and a missing or
		// unreadable file leaves the members exactly as they are (defaults for a fresh object). Repeated loads do not
		// accumulate duplicate entries.
		void Load(const tString& filename);

		// Writes the members to the config file (creating or overwriting it). Returns false if the file could not be
		// written.
		bool Save(const tString& filename) const;

		tList<MarketEntry>  Markets;			// [TICKER:CURR MIC] entries
		double              FeeThreshold;		// maximum fee before a group must absorb more legs
		int                 LookaheadDays;		// maximum day span before a group must close
		tList<tStringItem>  Ignored;			// tickers dropped from the output
		tList<tStringItem>  Reviewed;			// tickers kept (reviewed) after a Norbert's Gambit prompt

	private:
		static const double DefaultFeeThreshold;
		static const int    DefaultLookaheadDays;
	};
}
