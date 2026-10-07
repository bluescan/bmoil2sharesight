// BmoIL2ShareSight.cpp
//
// BmoIL2ShareSight is a command-line tool that converts an Investorline transaction-history CSV export into the
// bulk-trades CSV format that ShareSight imports.
//
// The input file has this structure (first line is an export-metadata row, second is the header, third is a dashes
// separator, the rest are data rows):
//   Transaction Date,Settlement Date,Activity Description,Description,Symbol,Quantity,Price,Price Currency,Total Amount,Total Currency
//
// The output file matches ShareSight's bulk_trades template:
//   Trade Date,Instrument Code,Market Code,Quantity,Price,Transaction Type,Exchange Rate (optional),
//   Brokerage (optional),Brokerage Currency (optional),Comments (optional)
//
// Only Buy and Sell activities are converted (those are the only transaction types the bulk-trades format supports).
// Every other activity is reported as skipped.
//
// Market codes: the input has no Market Code column, and different tickers -- or the same ticker traded in different
// currencies -- can live on different exchanges, so the code is recorded per (ticker, currency) pair in the
// bmoil2sharesight.cfg config file (next to the executable). The config file uses the tScript s-expr notation: a
// "Markets" list of [TICKER:CURR MIC] entries. The MIC is a standard ISO 10383 market identifier
// code (XNYS, ARCX, XNAS, XTSE, XTSX, NEOE); the matching ShareSight Market Code (NYSE, NASDAQ, TSX, TSXV, NEO) is what
// gets saved in the output CSV. When a pair is not recorded, the tool lists the supported MICs and asks the user to pick
// one by number, then saves the choice to the config file so it is not asked again.
//
// Brokerage fees: a single order may be split across several legs (rows) that share activity, symbol, transaction date,
// settlement date and price currency -- limit-order fills can settle at slightly different prices, and one or more legs
// may carry a Total Amount of 0 while a sibling carries the whole order's total. The fee is computed over the WHOLE
// group: a buy's total is gross + fee, a sell's total is gross - fee, so:
//
// fee = max(0, +/- (sum(|Total Amount|) - sum(|Quantity| * Price)))
//
// with the sign depending on the side. The fee is placed on the group's LAST non-zero-total leg -- the leg Investorline
// uses to finalize the order, the one carrying the order's total -- falling back to the first zero-total leg, then to
// the last leg, and is left blank when it is zero. A standalone leg is just a single-leg group.
//
// Ignored tickers (eg. Norbert's Gambit vehicles): a symbol dropped from the output is recorded in an "Ignored" list in
// the bmoil2sharesight.cfg config file (next to the executable, alongside the market codes) so it is remembered for
// future runs. Norbert's Gambit is detected automatically -- the same ticker bought in one currency and sold in another
// on the same day with matching share counts (eg. buy a CUS/DR in CAD on the TSX and sell it in USD on a US venue) --
// and the tool asks whether to ignore it. A "keep" answer is recorded in a "Reviewed" list so it is not asked again.
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

#include "Version.cmake.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif
#include <cstdlib>
#include <cstring>
#include <Foundation/tString.h>
#include <System/tCSV.h>
#include <System/tCmdLine.h>
#include <System/tFile.h>
#include <System/tPrint.h>
#include <System/tScript.h>


namespace Bmoil2ShareSightVersion
{
	int Major = 0;
	int Minor = 0;
	int Revision = 0;
	bool Parsed = false;
}


Bmoil2ShareSightVersion::Parser::Parser(const char* verStr)
{
	if (Parsed)
		return;

	// verStr is the stringized text of the set(...) call in Version.cmake.h, which is shared with CMake:
	// at runtime it looks like  "BMOIL2SHARESIGHT_VERSION" "0.1.0"  (CMake variable name and quoted version,
	// the quotes included). ExtractLeft('_') drops the name prefix and returns it; what we want is the
	// REMAINDER left in the string (VERSION" "0.1.0"), so the return value is deliberately discarded.
	// Exploding on '.' then yields three components, whose integral values are the version numbers.
	tString vstr(verStr);
	vstr.ExtractLeft('_');

	tList<tStringItem> components;
	tStd::tExplode(components, vstr, '.');

	tStringItem* comp = components.First();		Major = comp->GetAsInt(10);
	comp = comp->Next();						Minor = comp->GetAsInt(10);
	comp = comp->Next();						Revision = comp->GetAsInt(10);
	Parsed = true;
}


// Command line interface. Parameters and options are declared here (outside main) so that the tool's
// options are self-documenting and the parser populates them before main runs.
//
// The namespace holds only declarations; every function is defined after the closing brace, scoped
// with BMO2SS::.
namespace BMO2SS
{
	tCmdLine::tParam ParamInput("Investorline transaction-history CSV to read (input)", "Input", 1);
	tCmdLine::tParam ParamOutput("ShareSight bulk-trades CSV to write (output). Defaults to the input name with _sharesight.csv.", "Output", 2);
	tCmdLine::tOption OptionHelp("Display help.", "help", 'h');

	// One leg of a (possibly multi-leg) buy/sell trade, kept in input order.
	struct TradeLeg
	{
		int inRow;						// 1-based row in the input file (for warnings)
		tString tradeDate;
		tString settleDate;
		tString symbol;
		tString currency;				// the Price Currency of the leg (part of the (ticker, currency) market key)
		tString quantity;
		tString price;
		const char* transactionType;	// "BUY" or "SELL"
		bool zeroTotal;					// Total Amount cell was empty or 0
		double totalAbs;				// |Total Amount| (0 when the cell is empty)
		double gross;					// |Quantity| * Price
		tString brokerage;				// computed fee; empty == left blank
		tString groupKey;				// activity|SYMBOL|tradeDate|settleDate|CURRENCY (upper-cased)
	};

	// One market-code mapping: a ticker -- optionally narrowed to a currency -- to an ISO 10383 MIC. Lives on a tList
	// (an intrusive list, so it derives from tLink; see the PairCount notes above).
	struct MarketEntry : tLink<MarketEntry>
	{
		tString ticker;		// upper-case, eg. "MSFT"
		tString currency;		// upper-case ISO 4217 currency (eg. "USD"), or empty for any currency
		tString mic;			// upper-case ISO 10383 MIC, eg. "XNYS"
	};

	// Replaces the file extension of the supplied path with "_sharesight.csv".
	tString DefaultOutputName(const tString& inputName);

	// The config file remembers the market codes and the ignored/kept tickers across runs. It lives next to
	// the executable:
	//   <exe-dir>/bmoil2sharesight.cfg
	// In s-expr format: "Markets" entries are [TICKER:CURR MIC] (MIC is an ISO 10383 code);
	// "Ignored" entries are tickers dropped from the output; "Reviewed" entries are tickers
	// flagged as a possible Norbert's Gambit and kept (so the tool does not ask again).
	// Legacy KEY=VALUE format (TICKER.CURRENCY=MIC) is also read for backward compatibility.
	tString ConfigFile();

	// True if the symbol is on the ignore list (case-insensitive).
	bool IsIgnored(const tString& symbol, tList<tStringItem>& ignored);

	// True if the symbol is on the kept (reviewed) list -- flagged as a Norbert's Gambit and kept -- case-insensitive.
	bool IsReviewed(const tString& symbol, tList<tStringItem>& reviewed);

	// True when the config file is in the tScript s-expr format (any line whose first non-blank char is '['); otherwise
	// it is the legacy "TICKER[.CURRENCY]=MIC" / plain-ticker format.
	bool IsExprConfig(const tString& text);

	// Parse the s-expr config file into the market entries and the ignore/reviewed lists.
	void LoadConfigExpr(const tString& text, tList<MarketEntry>& markets, tList<tStringItem>& ignored, tList<tStringItem>& reviewed);

	// Parse the legacy config file (the format used before the s-expr one) into the same lists.
	void LoadConfigLegacy(const tString& text, tList<MarketEntry>& markets, tList<tStringItem>& ignored, tList<tStringItem>& reviewed);

	// Load the market codes and the ignore/kept lists from the config file, if the file exists. Tries the s-expr format
	// first and falls back to the legacy format for files written before the switch.
	void LoadConfig(tList<MarketEntry>& markets, tList<tStringItem>& ignored, tList<tStringItem>& reviewed);

	// Record a ticker in one of the two ticker lists (case-insensitive); returns true if it was not already listed
	// (i.e. the config file must be saved).
	bool AddIgnored(tList<tStringItem>& ignored, const tString& ticker);
	bool AddReviewed(tList<tStringItem>& reviewed, const tString& ticker);

	// Persist the market codes and the ignore/kept lists to the config file in the tScript s-expr format.
	bool SaveConfig(tList<MarketEntry>& markets, tList<tStringItem>& ignored, tList<tStringItem>& reviewed);

	// Convert a MIC to the ShareSight Market Code that is saved in the output CSV. Returns an empty
	// string when the MIC is not one of the supported codes (see the definition below).
	tString MarketCodeFromMIC(const tString& mic);

	// The ShareSight Market Code recorded for a (ticker, currency) pair; falls back from
	// TICKER.CURRENCY to TICKER. Empty when the pair is not recorded.
	tString MarketCodeFor(tList<MarketEntry>& markets, const tString& symbol, const tString& currency);

	// True when a market code can be asked for: stdin is an interactive terminal, or piped input that
	// already has data (an empty pipe cannot answer, so the prompt would only block).
	bool CanPromptForMarket();

	// Ask (numbered menu) which market a (ticker, currency) pair traded on, record the choice in the
	// market list and save the config file. Returns false to abort the run.
	bool PromptForMarket
	(
		tList<MarketEntry>& markets, tList<tStringItem>& ignored, tList<tStringItem>& reviewed,
		const tString& symbol, const tString& currency
	);

	// A (ticker, currency) pair present in the input, with a count of the matching buy/sell rows.
	// Derives from tLink so it can live on a tList (intrusive list; see the tList.h notes).
	struct PairCount : tLink<PairCount>
	{
		tString symbol;
		tString currency;
		unsigned long count;
	};

	// A symbol suspected of Norbert's Gambit: on at least one day its total Buy quantity in one currency equals
	// its total Sell quantity in a different currency. Currencies are kept in the order first seen in the input.
	// Derives from tLink so it can live on a tList.
	struct NorbertSuspect : tLink<NorbertSuspect>
	{
		tString symbol;
		tList<tStringItem> currencies;
		unsigned long totalRows;   // buy/sell rows for this symbol (across all its currencies/days)
	};

	// Detect Norbert's Gambit: symbols bought in one currency and sold in another on the same day with matching
	// share counts, excluding tickers already on the ignore or kept lists. One suspect per symbol (the first
	// matching day), in first-seen order.
	void FindNorbertSuspects
	(
		const TradeLeg* legs, int numLegs, tList<tStringItem>& ignored,
		tList<tStringItem>& reviewed, tList<NorbertSuspect>& suspects
	);

	// Ask the user to ignore or keep each suspected Norbert's Gambit symbol, updating the ignore/kept lists and
	// saving the config file as the choices come in.
	void PromptForNorbert
	(
		tList<MarketEntry>& markets, tList<tStringItem>& ignored, tList<tStringItem>& reviewed,
		const tList<NorbertSuspect>& suspects
	);

	// The legs live in raw (realloc'd) memory; destruct the legs that were constructed in place, then free.
	void FreeLegs(TradeLeg* legs, int numLegs);

	// Compute the brokerage for every leg (see the definition below for the multi-leg rules).
	void ComputeBrokerages(TradeLeg* legs, int numLegs);
}


tString BMO2SS::DefaultOutputName(const tString& inputName)
{
	// ExtractLeft('.') returns the part before the first '.' (and leaves the remainder in the string).
	// Use the return value, falling back to the whole name when the input has no extension.
	tString name(inputName);
	tString base = name.ExtractLeft('.');
	if (base.IsEmpty())
		base = name;

	return base + "_sharesight.csv";
}


tString BMO2SS::ConfigFile()
{
	return tSystem::tGetProgramDir() + "bmoil2sharesight.cfg";
}


bool BMO2SS::IsIgnored(const tString& symbol, tList<tStringItem>& ignored)
{
	for (tStringItem* item = ignored.First(); item; item = item->Next())
		if (symbol.IsEqualCI(*item))
			return true;

	return false;
}


bool BMO2SS::IsReviewed(const tString& symbol, tList<tStringItem>& reviewed)
{
	for (tStringItem* item = reviewed.First(); item; item = item->Next())
	{
		if (symbol.IsEqualCI(*item))
			return true;
	}

	return false;
}


bool BMO2SS::IsExprConfig(const tString& text)
{
	tList<tStringItem> lines;
	tStd::tExplode(lines, text, '\n');
	for (tStringItem* item = lines.First(); item; item = item->Next())
	{
		tString line = *item;
		line.Trim();
		if (line.Length() > 0 && line[0] == '[')
			return true;
	}

	return false;
}


void BMO2SS::LoadConfigExpr
(
	const tString& text, tList<MarketEntry>& markets, tList<tStringItem>& ignored, tList<tStringItem>& reviewed
)
{
	// A malformed config should not abort the run; tScript signals parse errors by throwing, so swallow them and
	// leave the lists as-is.
	try
	{
		tExprReader reader(text, false);

		// The file is a sequence of top-level blocks, each headed by a command word: Markets, Ignored or Reviewed.
		for (tExpression block = reader.First(); block.Valid(); block = block.Next())
		{
			tString cmd = block.Command().GetAtomString();

			if (cmd.IsEqualCI("Markets"))
			{
				// Each entry is [ TICKER:CURR MIC ] (new) or [ TICKER MIC ] / [ TICKER CURRENCY MIC ] (legacy).
				for (tExpression e = block.Item1(); e.Valid(); e = e.Next())
				{
					int n = e.CountItems();
					if (n < 2)
						continue;

					tString first = e.Item0().GetAtomString();
					tString mic	= e.ItemN(n - 1).GetAtomString();

					tString ticker;
					tString cur;
					int colon = -1;
					for (int i = 0; i < first.Length(); ++i)
					{
						if (first[i] == ':')
						{
							colon = i;
							break;
						}
					}
					if (colon > 0)
					{
						ticker = first.Left(colon);
						cur = first.Mid(colon + 1, first.Length() - colon - 1);
					}
					else if (n >= 3)
					{
						ticker = first;
						cur = e.Item1().GetAtomString();
					}
					else
					{
						ticker = first;
					}

					ticker.Trim().ToUpper();
					cur.Trim().ToUpper();
					mic.Trim().ToUpper();
					if (ticker.IsEmpty() || mic.IsEmpty() || MarketCodeFromMIC(mic).IsEmpty())
					{
						tPrintf("Warning: ignoring an invalid or unsupported\n");
						tPrintf("market entry in the config file.\n");
						continue;
					}

					MarketEntry* entry = new MarketEntry();
					entry->ticker = ticker;
					entry->currency = cur;
					entry->mic = mic;
					markets.Append(entry);
				}
			}
			else if (cmd.IsEqualCI("Ignored"))
			{
				for (tExpression e = block.Item1(); e.Valid(); e = e.Next())
				{
					tString t = e.GetAtomString();
					t.Trim();
					if (!t.IsEmpty())
						ignored.Append(new tStringItem(t));
				}
			}
			else if (cmd.IsEqualCI("Reviewed"))
			{
				for (tExpression e = block.Item1(); e.Valid(); e = e.Next())
				{
					tString t = e.GetAtomString();
					t.Trim();
					if (!t.IsEmpty())
						reviewed.Append(new tStringItem(t));
				}
			}
		}
	}
	catch (...)
	{
		// Leave the lists as-is; a later successful save will rewrite a valid file.
	}
}


void BMO2SS::LoadConfigLegacy
(
	const tString& text, tList<MarketEntry>& markets, tList<tStringItem>& ignored, tList<tStringItem>& reviewed
)
{
	tList<tStringItem> lines;
	tStd::tExplode(lines, text, '\n');
	for (tStringItem* item = lines.First(); item; item = item->Next())
	{
		tString line = *item;
		line.Trim();
		if (line.IsEmpty() || line[0] == '#')
			continue;

		// A line carrying '=' is a market-code entry (TICKER=MIC or TICKER.CURRENCY=MIC). Anything else is a
		// ticker: a plain one is ignored, one prefixed with '!' was flagged as a Norbert's Gambit and kept.
		bool hasEquals = false;
		for (int i = 0; i < line.Length(); ++i)
		{
			if (line[i] == '=')
			{
				hasEquals = true;
				break;
			}
		}

		if (!hasEquals)
		{
			if (line[0] == '!')
			{
				reviewed.Append(new tStringItem(line.Mid(1, (int)line.Length() - 1)));
			}
			else
			{
				ignored.Append(new tStringItem(line));
			}
			continue;
		}

		// Split "TICKER(.CURRENCY)=MIC". ExtractLeft('=') returns the left part; Right('=') is non-destructive.
		tString keySrc = line;
		tString micSrc = line;
		tString key = keySrc.ExtractLeft('=');
		key.Trim().ToUpper();
		tString mic = micSrc.Right('=');
		mic.Trim().ToUpper();
		if (key.IsEmpty() || mic.IsEmpty() || MarketCodeFromMIC(mic).IsEmpty())
		{
			tPrintf("Warning: ignoring an invalid or unsupported market entry in the config file.\n");
			continue;
		}

		MarketEntry* entry = new MarketEntry();
		entry->ticker = key;
		entry->mic = mic;
		int dot = -1;
		for (int i = 0; i < key.Length(); ++i)
		{
			if (key[i] == '.')
			{
				dot = i;
				break;
			}
		}
		if (dot > 0)
		{
			entry->ticker = key.Left(dot);
			entry->currency = key.Mid(dot + 1, key.Length() - dot - 1);
		}
		markets.Append(entry);
	}
}


void BMO2SS::LoadConfig(tList<MarketEntry>& markets, tList<tStringItem>& ignored, tList<tStringItem>& reviewed)
{
	tString text;
	if (!tSystem::tLoadFile(ConfigFile(), text))
		return;

	if (IsExprConfig(text))
		LoadConfigExpr(text, markets, ignored, reviewed);
	else
		LoadConfigLegacy(text, markets, ignored, reviewed);
}


bool BMO2SS::AddIgnored(tList<tStringItem>& ignored, const tString& ticker)
{
	tString clean = ticker;
	clean.Trim();
	if (clean.IsEmpty() || IsIgnored(clean, ignored))
		return false;

	ignored.Append(new tStringItem(clean));
	return true;
}


bool BMO2SS::AddReviewed(tList<tStringItem>& reviewed, const tString& ticker)
{
	tString clean = ticker;
	clean.Trim();
	if (clean.IsEmpty() || IsReviewed(clean, reviewed))
		return false;

	reviewed.Append(new tStringItem(clean));
	return true;
}


bool BMO2SS::SaveConfig(tList<MarketEntry>& markets, tList<tStringItem>& ignored, tList<tStringItem>& reviewed)
{
	// The writer creates (or overwrites) the file. It is written in the tScript s-expr notation.
	tExprWriter writer(ConfigFile());

	writer.Rem("BmoIL2ShareSight configuration (tScript s-expr format).");
	writer.Rem("Markets: [TICKER:CURR MIC]; the MIC is an ISO 10383 code");
	writer.Rem("converted to the ShareSight Market Code in the output CSV.");

	writer.Rem("Markets");
	writer.Begin();
	writer.Atom("Markets");
	writer.Indent();
	writer.CR();
	for (MarketEntry* m = markets.First(); m; m = m->Next())
	{
		tString key = m->ticker;
		if (!m->currency.IsEmpty())
		{
			key += ':';
			key += m->currency;
		}
		writer.Comp(key, m->mic);
	}
	writer.Dedent();
	writer.CR();
	writer.End();

	if (ignored.First())
	{
		writer.CR();
		writer.CR();
		writer.Rem("Ignored");
		writer.Rem("Tickers dropped from the output (eg. a Norbert's Gambit vehicle).");
		writer.Begin();
		writer.Atom("Ignored");
		writer.Indent();
		writer.CR();
		for (tStringItem* item = ignored.First(); item; item = item->Next())
			writer.Atom(*item);
		writer.Dedent();
		writer.CR();
		writer.End();
	}

	if (reviewed.First())
	{
		writer.Rem("Reviewed");
		writer.Rem("Flagged as a possible Norbert's Gambit and kept, so not asked again.");
		writer.Begin();
		writer.Atom("Reviewed");
		writer.Indent();
		writer.CR();
		for (tStringItem* item = reviewed.First(); item; item = item->Next())
			writer.Atom(*item);
		writer.Dedent();
		writer.CR();
		writer.End();
	}

	return true;
}


void BMO2SS::FindNorbertSuspects
(
	const TradeLeg* legs, int numLegs, tList<tStringItem>& ignored,
	tList<tStringItem>& reviewed, tList<NorbertSuspect>& suspects
)
{
	if (numLegs < 2)
	{
		return;
	}

	// First pass: collect the (ticker, currency) pairs present in the input, in the order first seen, and count the
	// number of buy/sell rows for each. Tickers already on the ignore or kept lists are skipped (their rows are
	// dropped -- or already accepted -- anyway, so there is nothing to offer).
	tList<PairCount> pairs;
	for (int i = 0; i < numLegs; ++i)
	{
		tString symbol = legs[i].symbol;
		symbol.Trim().ToUpper();
		if (symbol.IsEmpty() || IsIgnored(symbol, ignored) || IsReviewed(symbol, reviewed))
		{
			continue;
		}

		tString currency = legs[i].currency;
		currency.Trim().ToUpper();
		if (currency.IsEmpty())
		{
			continue;
		}

		PairCount* found = nullptr;
		for (PairCount* p = pairs.First(); p; p = p->Next())
		{
			if (p->symbol.IsEqualCI(symbol) && p->currency.IsEqualCI(currency))
			{
				p->count += 1;
				found = p;
				break;
			}
		}
		if (!found)
		{
			found = new PairCount();
			found->symbol = symbol;
			found->currency = currency;
			found->count = 1;
			pairs.Append(found);
		}
	}

	// Second pass: a symbol is a candidate when, on some day, the total shares it bought in one currency equal the
	// total shares it sold in another (or the reverse). Record the first candidate found per symbol.
	for (PairCount* p = pairs.First(); p; p = p->Next())
	{
		// Skip symbols that already produced a suspect.
		bool listed = false;
		for (NorbertSuspect* s = suspects.First(); s; s = s->Next())
		{
			if (s->symbol.IsEqualCI(p->symbol))
			{
				listed = true;
				break;
			}
		}
		if (listed)
		{
			continue;
		}

		bool match = false;
		for (PairCount* q = pairs.First(); q && !match; q = q->Next())
		{
			// The other currency must be a different one for the same symbol.
			if (q == p || !q->symbol.IsEqualCI(p->symbol) || q->currency.IsEqualCI(p->currency))
			{
				continue;
			}

			// Compare, day by day, the buy/sell share totals of the two currencies. Whole-share counts are compared
			// with a small tolerance to absorb any floating-point rounding.
			for (int i = 0; i < numLegs && !match; ++i)
			{
				if (!legs[i].symbol.IsEqualCI(p->symbol))
				{
					continue;
				}
				const tString date = legs[i].tradeDate;

				double pBuy = 0.0, pSell = 0.0, qBuy = 0.0, qSell = 0.0;
				for (int j = 0; j < numLegs; ++j)
				{
					const TradeLeg& leg = legs[j];
					if (!leg.symbol.IsEqualCI(p->symbol) || !leg.tradeDate.IsEqualCI(date))
					{
						continue;
					}
					double qnt = leg.quantity.GetAsDouble();
					if (qnt < 0.0)
					{
						qnt = -qnt;
					}
					const bool isP = leg.currency.IsEqualCI(p->currency);
					const bool isQ = leg.currency.IsEqualCI(q->currency);
					if (isP && leg.transactionType[0] == 'B')
						pBuy += qnt;
					else if (isP)
						pSell += qnt;
					else if (isQ && leg.transactionType[0] == 'B')
						qBuy += qnt;
					else if (isQ)
						qSell += qnt;
				}

				const double tol = 1e-6;
				const bool buyVsSell = (pBuy > 0.0) && ((pBuy - qSell) > -tol) && ((pBuy - qSell) < tol);
				const bool sellVsBuy = (pSell > 0.0) && ((pSell - qBuy) > -tol) && ((pSell - qBuy) < tol);
				match = buyVsSell || sellVsBuy;
			}
		}

		if (!match)
		{
			continue;
		}

		// Record the suspect: the symbol, every currency it traded in, and its total number of buy/sell rows.
		NorbertSuspect* sus = new NorbertSuspect();
		sus->symbol = p->symbol;
		unsigned long total = 0;
		for (PairCount* pc = pairs.First(); pc; pc = pc->Next())
		{
			if (pc->symbol.IsEqualCI(p->symbol))
			{
				total += pc->count;
				sus->currencies.Append(new tStringItem(pc->currency));
			}
		}
		sus->totalRows = total;
		suspects.Append(sus);
	}
}


void BMO2SS::PromptForNorbert
(
	tList<MarketEntry>& markets, tList<tStringItem>& ignored,
	tList<tStringItem>& reviewed, const tList<NorbertSuspect>& suspects
)
{
	for (const NorbertSuspect* s = suspects.First(); s; s = s->Next())
	{
		// Build the currency list for the message (eg. "CAD, USD").
		tString curList;
		for (tStringItem* c = s->currencies.First(); c; c = c->Next())
		{
			if (curList.IsValid())
			{
				curList += ", ";
			}
			curList += *c;
		}

		if (!CanPromptForMarket())
		{
			tPrintf
			(
				"\nPossible Norbert's Gambit detected for %s (%s, %u row(s)).\n"
				"It is not ignored and is converted as-is.\n"
				"Re-run in an interactive shell to decide.\n",
				s->symbol.Chr(), curList.Chr(), s->totalRows
			);
			continue;
		}

		tPrintf
		(
			"\nPossible Norbert's Gambit: %s was bought in one currency and sold in\n"
			"another (%s) on the same day with matching share counts (%u row(s)).\n"
			"Such legs are usually the two halves of the gambit, which are not real\n"
			"trades for ShareSight.\n",
			s->symbol.Chr(), curList.Chr(), s->totalRows
		);

		char buf[32];
		tString choice;
		while (1)
		{
			tPrintf("Ignore %s? [y/N]: ", s->symbol.Chr());
			fflush(stdout);
			if (!fgets(buf, (int)sizeof(buf), stdin))
			{
				AddReviewed(reviewed, s->symbol);
				tPrintf("No input; keeping %s in the output (not asked again).\n", s->symbol.Chr());
				break;
			}

			choice = buf;
			choice.Trim();
			if (choice.IsEqualCI("y") || choice.IsEqualCI("yes"))
			{
				AddIgnored(ignored, s->symbol);
				tPrintf("Ignoring %s from the output.\n", s->symbol.Chr());
				break;
			}
			if (choice.IsEmpty() || choice.IsEqualCI("n") || choice.IsEqualCI("no"))
			{
				AddReviewed(reviewed, s->symbol);
				tPrintf("Keeping %s in the output (not asked again).\n", s->symbol.Chr());
				break;
			}
			tPrintf("Please answer 'y' to ignore or 'n' to keep.\n");
		}

		if (!SaveConfig(markets, ignored, reviewed))
		{
			tPrintf("Warning: could not save the config file; choice for %s\n", s->symbol.Chr());
			tPrintf("will be asked again.\n");
		}
	}
}


tString BMO2SS::MarketCodeFromMIC(const tString& mic)
{
	tString code = mic;
	code.Trim();

	// The mapping follows the "Market Identifier Code (MIC) to Sharesight's Market Code" table in Data/Readme.txt.
	if (code.IsEqualCI("XTSE"))
		return "TSX";
	if (code.IsEqualCI("XTSX"))
		return "TSXV";
	if (code.IsEqualCI("NEOE"))
		return "NEO";
	if (code.IsEqualCI("XNAS"))
		return "NASDAQ";
	if (code.IsEqualCI("XNYS") || code.IsEqualCI("ARCX"))
		return "NYSE";

	return tString();
}


tString BMO2SS::MarketCodeFor(tList<MarketEntry>& markets, const tString& symbol, const tString& currency)
{
	tString key = symbol;
	key.Trim().ToUpper();
	if (key.IsEmpty())
		return tString();

	tString cur;
	if (!currency.IsEmpty())
	{
		cur = currency;
		cur.Trim().ToUpper();
	}

	// Prefer the exact (ticker, currency) pair; fall back to the ticker alone.
	for (MarketEntry* m = markets.First(); m; m = m->Next())
	{
		if (!m->ticker.IsEqualCI(key))
			continue;
		if (cur.IsEmpty() || m->currency.IsEmpty() || m->currency.IsEqualCI(cur))
			return MarketCodeFromMIC(m->mic);
	}

	return tString();
}


bool BMO2SS::CanPromptForMarket()
{
#if defined(_WIN32)
	if (_isatty(_fileno(stdin)))
		return true;
#else
	if (isatty(0))
		return true;
#endif

	// Not an interactive terminal: still answerable when input is piped in and has data available.
	// An empty (immediately-EOF) pipe cannot answer, so fail fast instead of blocking forever.
	int c = fgetc(stdin);
	if (c == EOF)
		return false;
	ungetc(c, stdin);
	return true;
}


bool BMO2SS::PromptForMarket
(
	tList<MarketEntry>& markets, tList<tStringItem>& ignored, tList<tStringItem>& reviewed,
	const tString& symbol, const tString& currency
)
{
	// The supported MICs in a stable order; the menu numbers follow this list. desc is the long
	// market name shown in the menu so the user can tell the entries apart (eg. XNYS vs ARCX) --
	// kept in sync with the table in Data/Readme.txt.
	static const struct
	{
		const char* mic;
		const char* code;
		const char* desc;
	} menu[] =
	{
		{ "XTSE", "TSX",	"Toronto Stock Exchange" },
		{ "XTSX", "TSXV",	"Toronto Venture Stock Exchange" },
		{ "NEOE", "NEO",	"CBOE-Canada/NEO Exchange" },
		{ "XNAS", "NASDAQ",	"The NASDAQ Exchange" },
		{ "XNYS", "NYSE",	"New York Stock Exchange" },
		{ "ARCX", "NYSE",	"New York Stock Exchange Arca" },
	};

	static const int menuCount = (int)(sizeof(menu) / sizeof(menu[0]));

	tString key = symbol;
	key.Trim().ToUpper();
	if (!currency.IsEmpty())
	{
		key += ':';
		tString cur = currency;
		cur.Trim().ToUpper();
		key += cur;
	}

	while (1)
	{
		tPrintf("\nWhich market did %s trade on?\n", key.Chr());
		for (int i = 0; i < menuCount; ++i)
			tPrintf("  %d. %s  (ShareSight: %-6s) -- %s\n", i + 1, menu[i].mic, menu[i].code, menu[i].desc);

		tPrintf("Enter 1-%d, or q to cancel: ", menuCount);
		fflush(stdout);

		char buf[32];
		if (!fgets(buf, (int)sizeof(buf), stdin))
		{
			tPrintf("\nCancelled: no input available. The market code for %s was not saved.\n", key.Chr());
			return false;
		}

		tString choice = buf;
		choice.Trim();
		if (choice.IsEqualCI("q"))
		{
			tPrintf("Cancelled: the market code for %s was not saved.\n", key.Chr());
			return false;
		}

		if (!(choice.IsValid() && choice.IsNumeric(true)))
		{
			tPrintf("'%s' is not a valid choice. Enter a number 1-%d.\n", choice.Chr(), menuCount);
			continue;
		}

		int index = atoi(choice.Chr());
		if (index < 1 || index > menuCount)
		{
			tPrintf("%d is out of range. Enter a number 1-%d.\n", index, menuCount);
			continue;
		}

		MarketEntry* entry = new MarketEntry();
		entry->ticker = symbol;
		entry->ticker.Trim().ToUpper();
		entry->currency = currency;
		entry->currency.Trim().ToUpper();
		entry->mic = menu[index - 1].mic;
		markets.Append(entry);

		if (SaveConfig(markets, ignored, reviewed))
		{
			tPrintf("Saved %s %s (ShareSight Market Code: %s).\n", key.Chr(), menu[index - 1].mic, menu[index - 1].code);
			tPrintf("Config file:\n%s\n", ConfigFile().Chr());
		}
		else
		{
			tPrintf("Warning: could not save the market code; it will be needed\n");
			tPrintf("again next run.\n");
			tPrintf("Config file:\n%s\n", ConfigFile().Chr());
		}

		return true;
	}

	return false;
}


void BMO2SS::FreeLegs(TradeLeg* legs, int numLegs)
{
	for (int i = 0; i < numLegs; ++i)
		legs[i].~TradeLeg();

	free(legs);
}


// Compute the brokerage fee for every leg.
//
// Legs that share activity, symbol, transaction date, settlement date, and price currency are one order
// (a multi-leg trade). For a group the fee is computed from the whole group's numbers. A buy's total
// amount is gross plus fees (fee = sum(|Total|) - sum(|Qty| * Price)); a sell's total is gross minus fees
// (fee = sum(|Qty| * Price) - sum(|Total|)). Negative results clamp to zero:
//     BUY : fee = max(0, sum(|Total Amount|) - sum(|Quantity| * Price))
//     SELL: fee = max(0, sum(|Quantity| * Price) - sum(|Total Amount|))
// The fee is placed on the group's LAST non-zero-total leg -- that is the leg Investorline uses to
// "finalize" the order, the one that carries the order's total while the other legs' totals simply
// weren't recorded. If every leg in the group has a zero total (malformed order), fall back to the
// first zero-total leg, then to the group's last leg. A standalone leg is just a single-leg group.
void BMO2SS::ComputeBrokerages(TradeLeg* legs, int numLegs)
{
	for (int i = 0; i < numLegs; ++i)
	{
		// Gather the group's numbers.
		int firstZero	= -1;
		int lastLeg		= -1;
		int lastNonZero	= -1;
		double sumTotal	= 0.0;
		double sumGross	= 0.0;
		for (int j = 0; j < numLegs; ++j)
		{
			if (legs[j].groupKey != legs[i].groupKey)
				continue;

			sumTotal += legs[j].totalAbs;
			sumGross += legs[j].gross;
			lastLeg = j;
			if (legs[j].zeroTotal)
			{
				if (firstZero == -1)
					firstZero = j;
			}
			else
			{
				lastNonZero = j;
			}
		}

		// Fee direction depends on the side: buys pay gross + fee, sells receive gross - fee.
		double fee = (std::strcmp(legs[i].transactionType, "SELL") == 0)
				? sumGross - sumTotal
				: sumTotal - sumGross;
		if (fee < 0.0)
			fee = 0.0;
		fee = std::round(fee * 100.0) / 100.0;

		// Place the fee: on the last non-zero-total leg (the one Investorline uses to finalize the order), else on the
		// first zero-total leg, else on the group's last leg. Exactly one leg (this one) will see i == target, so the
		// fee is assigned exactly once per group.
		int target = (lastNonZero != -1) ? lastNonZero : ((firstZero != -1) ? firstZero : lastLeg);
		if (target != i)
			continue;

		if (fee > 0.005)
			legs[i].brokerage = tsrPrintf("%.2f", fee);
	}
}


int main(int argc, char** argv)
{
	tCmdLine::tParse(argc, argv);

	if (BMO2SS::OptionHelp)
	{
		tCmdLine::tPrintUsage
		(
			u8"Tristan Grimmer",
			u8"BmoIL2ShareSight converts an Investorline transaction-history CSV into the ShareSight bulk-trades format.",
			Bmoil2ShareSightVersion::Major, Bmoil2ShareSightVersion::Minor, Bmoil2ShareSightVersion::Revision
		);
		tCmdLine::tPrintSyntax();
		return 0;
	}

	if (!BMO2SS::ParamInput)
	{
		tPrintf("Error: No input file specified.\n");
		tCmdLine::tPrintSyntax();
		return 1;
	}

	tString inputFile = BMO2SS::ParamInput.Get();
	if (!tSystem::tFileExists(inputFile))
	{
		tPrintf("Error: Input file does not exist:\n");
		tPrintf("%s\n", inputFile.Chr());
		return 1;
	}

	tString outputFile = BMO2SS::ParamOutput ? BMO2SS::ParamOutput.Get()
														: BMO2SS::DefaultOutputName(inputFile);

	// If the output file already exists, ask whether to overwrite it.
	if (tSystem::tFileExists(outputFile))
	{
		if (!BMO2SS::CanPromptForMarket())
		{
			tPrintf("Error: output file already exists:\n");
			tPrintf("%s\n", outputFile.Chr());
			tPrintf("Delete it or use a different output path.\n");
			return 1;
		}

		tPrintf("\nOutput file already exists:\n");
		tPrintf("%s\n", outputFile.Chr());
		tPrintf("Overwrite? [y/N]: ");
		fflush(stdout);

		char buf[32];
		if (!fgets(buf, (int)sizeof(buf), stdin))
		{
			tPrintf("\nCancelled: no input available.\n");
			return 1;
		}

		tString choice = buf;
		choice.Trim();
		if (!choice.IsEqualCI("y") && !choice.IsEqualCI("yes"))
		{
			tPrintf("Cancelled: output file was not overwritten.\n");
			return 1;
		}
	}

	// Config: market codes (per ticker, or per ticker+currency), ignored tickers, and kept (reviewed) tickers,
	// remembered in a file next to the executable (tScript s-expr format). Market codes are added when the user
	// answers the prompt for a pair that is not recorded yet; ignored/kept tickers are recorded when the user
	// answers the Norbert's Gambit prompt.
	tList<BMO2SS::MarketEntry> marketMap;
	tList<tStringItem> ignoreList;
	tList<tStringItem> reviewedList;
	BMO2SS::LoadConfig(marketMap, ignoreList, reviewedList);

	// Load the Investorline export.
	tSystem::tCSV input;
	if (!input.LoadFile(inputFile))
	{
		tPrintf("Error: Could not load input CSV:\n");
		tPrintf("%s\n", inputFile.Chr());
		return 1;
	}

	int numRows = input.GetNumRows();
	if (numRows < 2)
	{
		tPrintf("Error: Input CSV has no data rows.\n");
		return 1;
	}

	// Find the header row (defensively; the Investorline format has the header on the second line).
	int headerRow = -1;
	for (int row = 0; row < numRows; ++row)
	{
		if (input.Get(row, 0) == "Transaction Date")
		{
			headerRow = row;
			break;
		}
	}

	if (headerRow == -1)
	{
		headerRow = 0;
		tPrintf("Warning: Could not find the 'Transaction Date' header row;\n");
		tPrintf("assuming the first line.\n");
	}

	// ---- Prepare the output document with the ShareSight bulk-trades header.
	tSystem::tCSV output;
	{
		tList<tStringItem> header;
		const char* headerCells[] =
		{
			"Trade Date",
			"Instrument Code",
			"Market Code",
			"Quantity",
			"Price",
			"Transaction Type",
			"Exchange Rate (optional)",
			"Brokerage (optional)",
			"Brokerage Currency (optional)",
			"Comments (optional)"
		};

		for (const char* cell : headerCells)
			header.Append(new tStringItem(cell));

		if (!output.SetRow(header, 0))
		{
			tPrintf("Error: Could not initialize the output document.\n");
			return 1;
		}
	}

	// Conversion pass 1: collect the Buy/Sell legs (multi-leg orders are grouped later for the fees).
	int converted = 0;
	int skipped = 0;
	int ignoredRows = 0;

	BMO2SS::TradeLeg* legs = nullptr;
	int numLegs = 0;
	int capLegs = 0;

	for (int row = headerRow + 1; row < numRows; ++row)
	{
		tString tradeDate = input.Get(row, 0);
		tradeDate.Trim();
		if (tradeDate.IsEmpty())
			continue;

		// Skip the dashes separator line the Investorline export includes right under the header.
		if (tradeDate[0] == '-')
			continue;

		tString settleDate	= input.Get(row, 1);
		settleDate.Trim();
		tString activity	= input.Get(row, 2);
		activity.Trim();
		tString symbol		= input.Get(row, 4);
		symbol.Trim();
		tString quantity	= input.Get(row, 5);
		quantity.Trim();
		tString price		= input.Get(row, 6);
		price.Trim();
		tString currency	= input.Get(row, 7);
		currency.Trim();
		tString total		= input.Get(row, 8);
		total.Trim();

		// Only Buy and Sell map to the bulk-trades transaction types ShareSight accepts.
		const char* transactionType = nullptr;
		if (activity.IsEqualCI((const char8_t*)"Buy"))
			transactionType = "BUY";
		else if (activity.IsEqualCI((const char8_t*)"Sell"))
			transactionType = "SELL";
		else
		{
			skipped++;
			tPrintf("Info: row %d: activity '%s' is not a Buy or Sell; skipped.\n", row + 1, activity.Chr());
			continue;
		}

		// Instrument Code is compulsory, and a missing one means this row does not map to a tradable instrument.
		if (symbol.IsEmpty())
		{
			skipped++;
			tPrintf("Warning: row %d: no Symbol for activity '%s'; skipped.\n", row + 1, activity.Chr());
			continue;
		}

		// Tickers on the ignore list (eg. Norbert's Gambit vehicles) are dropped entirely.
		if (BMO2SS::IsIgnored(symbol, ignoreList))
		{
			ignoredRows++;
			continue;
		}

		// Quantity is signed (Sells are negative), so allow a leading '-' as well as a decimal point.
		bool quantityOk = quantity.IsValid() && quantity.IsNumeric(true, true);

		if (!quantityOk)
		{
			skipped++;
			tPrintf("Warning: row %d: invalid Quantity '%s'; skipped.\n", row + 1, quantity.Chr());
			continue;
		}

		if (price.IsEmpty() || !price.IsNumeric(true))
		{
			skipped++;
			tPrintf("Warning: row %d: invalid Price '%s'; skipped.\n", row + 1, price.Chr());
			continue;
		}

		// Total Amount is signed (buys are negative), so allow a leading '-' as well as a decimal point.
		// A zero or blank total is fine: in a multi-leg order the sibling legs carry the order's total.
		bool totalValid = total.IsValid() && total.IsNumeric(true, true);

		double totalAbs = 0.0;
		if (totalValid)
		{
			totalAbs = total.GetAsDouble();
			if (totalAbs < 0.0)
				totalAbs = -totalAbs;
		}

		if (numLegs == capLegs)
		{
			int newCap = (capLegs == 0) ? 64 : capLegs * 2;
			BMO2SS::TradeLeg* newLegs =
				(BMO2SS::TradeLeg*)realloc(legs, newCap * sizeof(BMO2SS::TradeLeg));
			if (!newLegs)
			{
				tPrintf("Error: Out of memory.\n");
				BMO2SS::FreeLegs(legs, numLegs);
				return 1;
			}
			legs = newLegs;
			capLegs = newCap;
		}

		// Construct the leg in place (the array is raw realloc'd memory, not yet constructed).
		BMO2SS::TradeLeg* newLeg = new (&legs[numLegs]) BMO2SS::TradeLeg();
		BMO2SS::TradeLeg& leg = *newLeg;
		leg.inRow = row + 1;
		leg.tradeDate = tradeDate;
		leg.settleDate = settleDate;
		leg.symbol = symbol;
		leg.currency = currency;
		leg.quantity = quantity;
		leg.price = price;
		leg.transactionType = transactionType;
		leg.totalAbs = totalAbs;
		leg.zeroTotal = (leg.totalAbs <= 0.005);
		leg.gross = quantity.GetAsDouble() * price.GetAsDouble();
		if (leg.gross < 0.0)
			leg.gross = -leg.gross;

		// Group key: activity, symbol, transaction date, settlement date, price currency (case-insensitive).
		// Price is deliberately excluded -- legs of one order can fill at slightly different prices.
		leg.groupKey = transactionType;
		leg.groupKey += symbol.Upper();
		leg.groupKey += '|';
		leg.groupKey += tradeDate;
		leg.groupKey += '|';
		leg.groupKey += settleDate;
		leg.groupKey += '|';
		leg.groupKey += currency.Upper();

		numLegs++;
		converted++;
	}

	// ---- Norbert's Gambit: detect symbols bought in one currency and sold in another on the same day with
	// matching share counts, and ask whether to drop them. A "keep" is recorded so the same ticker is not asked
	// again. This runs before the market-code prompts so a dropped symbol is not asked about.
	{
		tList<BMO2SS::NorbertSuspect> suspects;
		BMO2SS::FindNorbertSuspects(legs, numLegs, ignoreList, reviewedList, suspects);
		BMO2SS::PromptForNorbert(marketMap, ignoreList, reviewedList, suspects);
	}

	// ---- Market codes: one per (ticker, currency) pair, recorded in the config file. Ask once per
	// pair that is not recorded yet; each answer is saved to the config file for future runs.
	for (int i = 0; i < numLegs; i++)
	{
		const BMO2SS::TradeLeg& leg = legs[i];
		if (BMO2SS::IsIgnored(leg.symbol, ignoreList))
			continue;
		if (!BMO2SS::MarketCodeFor(marketMap, leg.symbol, leg.currency).IsEmpty())
			continue;

		tString pair = leg.symbol;
		if (!leg.currency.IsEmpty())
			pair += tsrPrintf(" (%s)", leg.currency.Chr());

		if (!BMO2SS::CanPromptForMarket())
		{
			tPrintf("Error: no market code is recorded for %s, and stdin is not\n",
					pair.Chr());
			tPrintf("interactive.\n");
			tPrintf("Record it in the config file next to the executable\n");
			tPrintf("(s-expr format). Supported MICs:\n");
			tPrintf("XTSE, XTSX, NEOE, XNAS, XNYS, ARCX (see Data/Readme.txt).\n");
			tPrintf("Config file:\n%s\n", BMO2SS::ConfigFile().Chr());
			return 1;
		}

		if (!BMO2SS::PromptForMarket(marketMap, ignoreList, reviewedList, leg.symbol, leg.currency))
		{
			tPrintf("Error: aborted -- the output would carry %s rows without a Market Code.\n", pair.Chr());
			return 1;
		}
	}

	// Compute the fees (grouping multi-leg orders), then write the rows back in input order.
	BMO2SS::ComputeBrokerages(legs, numLegs);

	int outRow = 1;
	int written = 0;
	int droppedNorbert = 0;

	for (int i = 0; i < numLegs; i++)
	{
		const BMO2SS::TradeLeg& leg = legs[i];

		// A ticker the user just chose to ignore (a detected Norbert's Gambit) was collected before the prompt;
		// drop it from the output now.
		if (BMO2SS::IsIgnored(leg.symbol, ignoreList))
		{
			droppedNorbert++;
			continue;
		}

		tList<tStringItem> outRowCells;
		outRowCells.Append(new tStringItem(leg.tradeDate));
		outRowCells.Append(new tStringItem(leg.symbol));
		tString market = BMO2SS::MarketCodeFor(marketMap, leg.symbol, leg.currency);
		if (market.IsEmpty())
			tPrintf
			(
				"Warning: Row %d: no market code for %s; the Market Code cell is left blank.\n",
				leg.inRow, leg.symbol.Chr()
			);
		outRowCells.Append(new tStringItem(market));
		outRowCells.Append(new tStringItem(leg.quantity));
		outRowCells.Append(new tStringItem(leg.price));
		outRowCells.Append(new tStringItem(leg.transactionType));
		outRowCells.Append(new tStringItem());	// Exchange Rate
		outRowCells.Append(new tStringItem(leg.brokerage));
		outRowCells.Append(new tStringItem());	// Brokerage Currency
		outRowCells.Append(new tStringItem());	// Comments

		if (!output.SetRow(outRowCells, outRow))
		{
			tPrintf("Error: Could not write row %d to the output document.\n", outRow);
			BMO2SS::FreeLegs(legs, numLegs);
			return 1;
		}

		outRow++;
		written++;
	}

	BMO2SS::FreeLegs(legs, numLegs);

	if (written == 0)
	{
		tPrintf("Warning: No Buy or Sell rows were found; the output file\n");
		tPrintf("will contain only the header.\n");
	}

	// ---- Save the result.
	if (!output.SaveFile(outputFile))
	{
		tPrintf("Error: Could not save output CSV:\n");
		tPrintf("%s\n", outputFile.Chr());
		return 1;
	}

	tPrintf
	(
		"BmoIL2ShareSight V%d.%d.%d\n",
		Bmoil2ShareSightVersion::Major, Bmoil2ShareSightVersion::Minor, Bmoil2ShareSightVersion::Revision
	);

	tPrintf("Success: converted %d Buy/Sell row(s); %d row(s) skipped.\n", written, skipped);
	tPrintf("Input:\n%s\n", inputFile.Chr());
	tPrintf("Output:\n%s\n", outputFile.Chr());

	if (droppedNorbert > 0)
	{
		tPrintf("Info: ignored %d row(s) for the detected Norbert's Gambit ticker(s).\n", droppedNorbert);
		tPrintf("Config file:\n%s\n", BMO2SS::ConfigFile().Chr());
	}

	if (ignoredRows > 0)
	{
		tPrintf("Info: ignored %d row(s) for tickers on the ignore list.\n", ignoredRows);
		tPrintf("Config file:\n%s\n", BMO2SS::ConfigFile().Chr());
	}

	return 0;
}
