// Config.cpp
//
// Implementation of BMO2SS::Configuration: loads and saves the bmoil2sharesight.cfg file, written in the tScript
// s-expr notation.
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

#include "Config.h"
#include <Foundation/tString.h>
#include <System/tFile.h>
#include <System/tPrint.h>
#include <System/tScript.h>


const double	BMO2SS::Configuration::DefaultFeeThreshold		= 100.0;
const int		BMO2SS::Configuration::DefaultLookaheadDays		= 7;


BMO2SS::Configuration::Configuration() :
	FeeThreshold(DefaultFeeThreshold),
	LookaheadDays(DefaultLookaheadDays)
{
}


void BMO2SS::Configuration::Load(const tString& filename)
{
	// Reset first so a fresh object holds the defaults and a repeated load does not accumulate duplicates. A key the
	// file does not record therefore keeps its default.
	Markets.Clear();
	Ignored.Clear();
	Reviewed.Clear();
	FeeThreshold	= DefaultFeeThreshold;
	LookaheadDays	= DefaultLookaheadDays;

	tString text;
	if (!tSystem::tLoadFile(filename, text))
		return;		// No file (or unreadable): keep the defaults.

	// A malformed config should not abort the run; tScript signals parse errors by throwing, so swallow them and keep
	// whatever parsed cleanly.
	try
	{
		tExprReader reader(text, false);

		// The file is a sequence of top-level blocks, each headed by a command word: Markets, Options, Ignored or
		// Reviewed. Any other block is ignored (forward compatibility).
		for (tExpression block = reader.First(); block.Valid(); block = block.Next())
		{
			switch (block.Command().Hash())
			{
				case tHash::tHashCT("Markets"):
				{
					// Each entry is [TICKER:CURR MIC]
					for (tExpression e = block.Item1(); e.Valid(); e = e.Next())
					{
						int n = e.CountItems();
						if (n < 2)
							continue;

						tString mic = e.ItemN(n - 1);
						mic.Trim().ToUpper();
						if (mic.IsEmpty())
							continue;

						// The leading atom is always "TICKER:CURR"; a bare ticker (no currency) is skipped.
						tList<tStringItem> parts;
						if (tStd::tExplode(parts, e.Item0(), ':') < 2)
							continue;
						tString ticker	= *parts.First();
						tString cur		= *parts.Last();
						ticker.Trim().ToUpper();
						cur.Trim().ToUpper();
						if (ticker.IsEmpty())
							continue;

						MarketEntry* entry = new MarketEntry();
						entry->Ticker	= ticker;
						entry->Currency	= cur;
						entry->Mic		= mic;
						Markets.Append(entry);
					}
					break;
				}

				case tHash::tHashCT("Options"):
				{
					for (tExpression e = block.Item1(); e.Valid(); e = e.Next())
					{
						switch (e.Command().Hash())
						{
							case tHash::tHashCT("FeeThreshold"):	FeeThreshold	= e.Arg1(); break;
							case tHash::tHashCT("LookaheadDays"):	LookaheadDays	= e.Arg1(); break;
							// Any other option is ignored (forward compatibility).
						}
					}
					break;
				}

				case tHash::tHashCT("Ignored"):
				case tHash::tHashCT("Reviewed"):
				{
					tList<tStringItem>& list = (block.Command().Hash() == tHash::tHashCT("Ignored")) ? Ignored : Reviewed;
					for (tExpression e = block.Item1(); e.Valid(); e = e.Next())
					{
						tString t = e;
						t.Trim();
						if (!t.IsEmpty())
							list.Append(new tStringItem(t));
					}
					break;
				}
				// Any other top-level block is ignored (forward compatibility).
			}
		}
	}
	catch (...)
	{
		// Keep whatever parsed cleanly; a later successful save will rewrite a valid file.
	}
}


bool BMO2SS::Configuration::Save(const tString& filename) const
{
	// The writer creates (or overwrites) the file. It is written in the tScript s-expr notation.
	tExprWriter writer(filename);

	// Header comment block, then a blank line before the first section.
	writer.Rem("BmoIL2ShareSight configuration (tScript s-expr format).");
	writer.Rem("Markets: [TICKER:CURR MIC]; the MIC is an ISO 10383 code");
	writer.Rem("converted to the ShareSight Market Code in the output CSV.");
	writer.CR();

	// Markets (always written).
	writer.Begin();
	writer.Atom("Markets");
	writer.Indent();
	if (Markets.First())
	{
		writer.CR();
		MarketEntry* last = Markets.Last();
		for (MarketEntry* m = Markets.First(); m; m = m->Next())
		{
			tString key = m->Ticker + ":" + m->Currency;

			// Compose every entry but the last with a trailing newline (Comp), and the last without one (Coms), so
			// there is no blank line before the closing "]".
			if (m == last)
				writer.Coms(key, m->Mic);
			else
				writer.Comp(key, m->Mic);
		}
	}
	writer.Dedent();
	writer.CR();
	writer.End();

	// Blank line, then the Options section (always written).
	writer.CR();
	writer.Rem("Options");
	writer.Rem("FeeThreshold  : Max fee (in trade currency) before a group must absorb more legs.");
	writer.Rem("LookaheadDays : Max day span for a group before it must close.");
	writer.Begin();
	writer.Atom("Options");
	writer.Indent();
	writer.CR();
	writer.Comp("FeeThreshold", tsrPrintf("%g", FeeThreshold));
	writer.Coms("LookaheadDays", tsrPrintf("%d", LookaheadDays));
	writer.Dedent();
	writer.CR();
	writer.End();

	// Ignored (only when present).
	if (Ignored.First())
	{
		writer.CR();
		writer.Rem("Ignored");
		writer.Rem("Tickers dropped from the output (eg. a Norbert's Gambit vehicle).");
		writer.Begin();
		writer.Atom("Ignored");
		writer.Indent();
		writer.CR();
		for (tStringItem* item = Ignored.First(); item; item = item->Next())
			writer.Atom(*item);
		writer.Dedent();
		writer.CR();
		writer.End();
	}

	// Reviewed (only when present).
	if (Reviewed.First())
	{
		writer.CR();
		writer.Rem("Reviewed");
		writer.Rem("Flagged as a possible Norbert's Gambit and kept, so not asked again.");
		writer.Begin();
		writer.Atom("Reviewed");
		writer.Indent();
		writer.CR();
		for (tStringItem* item = Reviewed.First(); item; item = item->Next())
			writer.Atom(*item);
		writer.Dedent();
		writer.CR();
		writer.End();
	}

	return true;
}
