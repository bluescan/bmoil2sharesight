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
// Brokerage fees: a single order may be split across several trades (rows) that share ticker, direction and price
// currency -- limit-order fills can settle at slightly different prices over one or more days, and one or more trades
// may carry a Total Amount of 0 while a sibling carries the whole order's total.
//
// The tool processes trades sequentially (in CSV order, which is reverse-chronological). For each trade it computes
// the fee from the accumulated order. An order "closes" when its fee is in range: >= -1.0 (approximately zero,
// allowing for display rounding) and <= FeeThreshold (default $100). If a trade's fee is out of range (negative,
// above the threshold, or the total is zero), an order is opened; subsequent trades with the same ticker, direction
// and currency are absorbed (up to MaxLookaheadDays, default 7) until the fee closes. Multiple orders can be open at
// the same time. The fee is placed on the trade that closes the order (the "finalizing" trade). A standalone trade
// whose fee is already in range is a single-trade order.
//
// The fee for an order:
//   BUY : fee = sum(|Total Amount|) - sum(|Quantity| * Price)
//   SELL: fee = sum(|Quantity| * Price) - sum(|Total Amount|)
//
// FeeThreshold and LookaheadDays are read from (and written to) the config file after each run.
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
#include "Config.h"


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

	// One trade -- a single row (a fill) of the input -- kept in input order. Several trades of the same
	// ticker, direction and currency can belong to one order; the fee is computed over the whole order.
	struct Trade : tLink<Trade>
	{
		int InRow;						// 1-based row in the input file (for warnings)
		tString TradeDate;
		tString SettleDate;
		tString Symbol;
		tString Currency;				// the Price Currency of the trade (part of the (ticker, currency) market key)
		tString Quantity;
		tString Price;
		const char* TransactionType;	// "BUY" or "SELL"
		bool ZeroTotal;					// Total Amount cell was empty or 0
		double TotalAbs;				// |Total Amount| (0 when the cell is empty)
		double Gross;					// |Quantity| * Price
		tString Brokerage;				// computed fee; empty == left blank
	};

	// Replaces the file extension of the supplied path with "_sharesight.csv".
	tString DefaultOutputName(const tString& inputName);

	// The config file remembers the market codes and the ignored/kept tickers across runs. It lives next to
	// the executable:
	//   <exe-dir>/bmoil2sharesight.cfg
	// In s-expr format: "Markets" entries are [TICKER:CURR MIC] (MIC is an ISO 10383 code);
	// "Ignored" entries are tickers dropped from the output; "Reviewed" entries are tickers
	// flagged as a possible Norbert's Gambit and kept (so the tool does not ask again).
	tString ConfigFile();

	// True if the symbol is on the ignore list (case-insensitive).
	bool IsIgnored(const tString& symbol, tList<tStringItem>& ignored);

	// True if the symbol is on the kept (reviewed) list -- flagged as a Norbert's Gambit and kept -- case-insensitive.
	bool IsReviewed(const tString& symbol, tList<tStringItem>& reviewed);

	// Record a ticker in one of the two ticker lists (case-insensitive); returns true if it was not already listed
	// (i.e. the config file must be saved).
	bool AddIgnored(tList<tStringItem>& ignored, const tString& ticker);
	bool AddReviewed(tList<tStringItem>& reviewed, const tString& ticker);

	// Convert a MIC to the ShareSight Market Code that is saved in the output CSV. Returns an empty
	// string when the MIC is not one of the supported codes (see the definition below).
	tString MarketCodeFromMIC(const tString& mic);

	// The ShareSight Market Code recorded for a (ticker, currency) pair. Empty when the pair is not recorded.
	tString MarketCodeFor(tList<MarketEntry>& markets, const tString& symbol, const tString& currency);

	// True when a market code can be asked for: stdin is an interactive terminal, or piped input that
	// already has data (an empty pipe cannot answer, so the prompt would only block).
	bool CanPromptForMarket();

	// Ask (numbered menu) which market a (ticker, currency) pair traded on and record the choice in the
	// market list (the config file is persisted when the run exits). Returns false to abort the run.
	bool PromptForMarket
	(
		tList<MarketEntry>& markets, const tString& symbol, const tString& currency
	);

	// A (ticker, currency) pair present in the input, with a count of the matching buy/sell rows.
	// Derives from tLink so it can live on a tList (intrusive list; see the tList.h notes).
	struct PairCount : tLink<PairCount>
	{
		tString Symbol;
		tString Currency;
		int Count;
	};

	// A symbol suspected of Norbert's Gambit: on at least one day its total Buy quantity in one currency equals
	// its total Sell quantity in a different currency. Currencies are kept in the order first seen in the input.
	// Derives from tLink so it can live on a tList.
	struct NorbertSuspect : tLink<NorbertSuspect>
	{
		tString Symbol;
		tList<tStringItem> Currencies;
		int TotalRows;   // buy/sell rows for this symbol (across all its currencies/days)
	};

	// Detect Norbert's Gambit: symbols bought in one currency and sold in another on the same day with matching
	// share counts, excluding tickers already on the ignore or kept lists. One suspect per symbol (the first
	// matching day), in first-seen order.
	void FindNorbertSuspects
	(
		const tList<Trade>& trades, tList<tStringItem>& ignored,
		tList<tStringItem>& reviewed, tList<NorbertSuspect>& suspects
	);

	// Ask the user to ignore or keep each suspected Norbert's Gambit symbol, updating the ignore/kept lists
	// (the config file is persisted when the run exits).
	void PromptForNorbert
	(
		tList<tStringItem>& ignored, tList<tStringItem>& reviewed,
		const tList<NorbertSuspect>& suspects
	);

	// Convert an ISO date string ("YYYY-MM-DD") to a day number for comparison.
	int DateToDayNum(const tString& date);

	// Compute the brokerage for every trade (see the file header for the algorithm).
	bool ComputeBrokerages(tList<Trade>& trades, double feeThreshold, int lookaheadDays);

	// A reconstructed "order": NOT a real broker order and NOT a CSV row. It is a transient fee bucket the algorithm
	// builds in memory, accumulating the trades that settle together until the fee "closes". It is never persisted; it
	// exists only for the duration of ComputeBrokerages to work out the fee and the leg (the highest-|Total Amount|
	// trade, ties -> last) that will carry it. Derives from tLink so it can live on a tList.
	class Order : public tLink<Order>
	{
	public:
		tString Ticker;       // upper-case symbol
		tString Direction;    // "BUY" or "SELL"
		tString Currency;     // upper-case currency
		int DayNum;           // day number of the order's opening date
		int TradeCount;       // number of legs absorbed
		double SumTotal;      // running SUM of |Total Amount| (feeds Fee())
		double SumGross;      // running SUM of |Quantity| * Price (feeds Fee())
		double MaxTotal;      // running MAX of |Total Amount| (carrier selection)
		Trade* FeeCarrier;    // the max-|Total Amount| leg (ties -> last) that will receive the fee
		bool Closed;          // true once finalised (no longer absorbs trades)
		bool Place;           // true when the fee is in range and non-zero (the projection pass writes it)
		double PlacedFee;     // the rounded fee to place (meaningful only when Place is true)

		// Set the identity from the first leg and accumulate it.
		void Open(Trade& first)
		{
			Ticker = first.Symbol;
			Ticker.ToUpper();
			Direction = first.TransactionType;
			Currency = first.Currency;
			Currency.ToUpper();
			DayNum = DateToDayNum(first.TradeDate);
			TradeCount = 0;
			SumTotal = 0.0;
			SumGross = 0.0;
			MaxTotal = -1.0;
			FeeCarrier = nullptr;
			Closed = false;
			Place = false;
			PlacedFee = 0.0;
			Submit(first);
		}

		// Absorb one more leg: keep the running sums (fee) and the running max (carrier, ties -> last via >=).
		void Submit(Trade& t)
		{
			SumTotal += t.TotalAbs;
			SumGross += t.Gross;
			TradeCount++;
			if (t.TotalAbs >= MaxTotal)
			{
				FeeCarrier = &t;
				MaxTotal = t.TotalAbs;
			}
		}

		// True when this trade matches the order (same ticker, direction and currency).
		bool CanConsume(const Trade& t) const
		{
			return Ticker.IsEqualCI(t.Symbol) &&
			       Direction == t.TransactionType &&
			       Currency.IsEqualCI(t.Currency);
		}

		// The order's brokerage fee (BUY: sumTotal - sumGross; SELL: sumGross - sumTotal).
		double Fee() const
		{
			if (Direction[0] == 'S')
				return SumGross - SumTotal;
			return SumTotal - SumGross;
		}

		// True when the fee is within the acceptable band (approximately zero to feeThreshold).
		bool InRange(double feeThreshold) const
		{
			return (Fee() >= -1.0) && (Fee() <= feeThreshold);
		}

		bool IsClosed() const
		{
			return Closed;
		}

		// Finalise the order (it will no longer absorb trades). When 'place' is true the fee is in range and we record
		// it (blanking it if it rounds to ~0); otherwise the fee is left blank.
		void Finalize(bool place, double feeThreshold)
		{
			Closed = true;
			if (place && InRange(feeThreshold))
			{
				PlacedFee = std::round(Fee() * 100.0) / 100.0;
				Place = (PlacedFee > 0.005);
			}
			else
			{
				Place = false;
				PlacedFee = 0.0;
			}
		}
	};
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


void BMO2SS::FindNorbertSuspects
(
	const tList<Trade>& trades, tList<tStringItem>& ignored,
	tList<tStringItem>& reviewed, tList<NorbertSuspect>& suspects
)
{
	// Need at least two trades before there is any (ticker, currency) pair to compare.
	if (trades.Count() < 2)
		return;

	// First pass: collect the (ticker, currency) pairs present in the input, in the order first seen, and count the
	// number of buy/sell rows for each. Tickers already on the ignore or kept lists are skipped (their rows are
	// dropped -- or already accepted -- anyway, so there is nothing to offer).
	tList<PairCount> pairs;
	for (const Trade* trade = trades.First(); trade; trade = trade->Next())
	{
		tString symbol = trade->Symbol;
		symbol.Trim().ToUpper();
		if (symbol.IsEmpty() || IsIgnored(symbol, ignored) || IsReviewed(symbol, reviewed))
			continue;

		tString currency = trade->Currency;
		currency.Trim().ToUpper();
		if (currency.IsEmpty())
			continue;

		PairCount* found = nullptr;
		for (PairCount* p = pairs.First(); p; p = p->Next())
		{
			if (p->Symbol.IsEqualCI(symbol) && p->Currency.IsEqualCI(currency))
			{
				p->Count += 1;
				found = p;
				break;
			}
		}
		if (!found)
		{
			found = new PairCount();
			found->Symbol = symbol;
			found->Currency = currency;
			found->Count = 1;
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
			if (s->Symbol.IsEqualCI(p->Symbol))
			{
				listed = true;
				break;
			}
		}
		if (listed)
			continue;

		bool match = false;
		for (PairCount* q = pairs.First(); q && !match; q = q->Next())
		{
			// The other currency must be a different one for the same symbol.
			if (q == p || !q->Symbol.IsEqualCI(p->Symbol) || q->Currency.IsEqualCI(p->Currency))
				continue;

			// Compare, day by day, the buy/sell share totals of the two currencies. Whole-share counts are compared
			// with a small tolerance to absorb any floating-point rounding.
			for (const Trade* trade = trades.First(); trade && !match; trade = trade->Next())
			{
				if (!trade->Symbol.IsEqualCI(p->Symbol))
					continue;

				const tString date = trade->TradeDate;

				double pBuy = 0.0, pSell = 0.0, qBuy = 0.0, qSell = 0.0;
				for (const Trade* j = trades.First(); j; j = j->Next())
				{
					if (!j->Symbol.IsEqualCI(p->Symbol) || !j->TradeDate.IsEqualCI(date))
						continue;

					double qnt = j->Quantity.GetAsDouble();
					if (qnt < 0.0)
						qnt = -qnt;

					const bool isP = j->Currency.IsEqualCI(p->Currency);
					const bool isQ = j->Currency.IsEqualCI(q->Currency);
					if (isP && j->TransactionType[0] == 'B')
						pBuy += qnt;
					else if (isP)
						pSell += qnt;
					else if (isQ && j->TransactionType[0] == 'B')
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
			continue;

		// Record the suspect: the symbol, every currency it traded in, and its total number of buy/sell rows.
		NorbertSuspect* sus = new NorbertSuspect();
		sus->Symbol = p->Symbol;
		int total = 0;
		for (PairCount* pc = pairs.First(); pc; pc = pc->Next())
		{
			if (pc->Symbol.IsEqualCI(p->Symbol))
			{
				total += pc->Count;
				sus->Currencies.Append(new tStringItem(pc->Currency));
			}
		}
		sus->TotalRows = total;
		suspects.Append(sus);
	}
}


void BMO2SS::PromptForNorbert
(
	tList<tStringItem>& ignored, tList<tStringItem>& reviewed,
	const tList<NorbertSuspect>& suspects
)
{
	for (const NorbertSuspect* s = suspects.First(); s; s = s->Next())
	{
		// Build the currency list for the message (eg. "CAD, USD").
		tString curList;
		for (tStringItem* c = s->Currencies.First(); c; c = c->Next())
		{
			if (curList.IsValid())
				curList += ", ";

			curList += *c;
		}

		if (!CanPromptForMarket())
		{
			tPrintf
			(
				"\nPossible Norbert's Gambit detected for %s (%s, %u row(s)).\n"
				"It is not ignored and is converted as-is.\n"
				"Re-run in an interactive shell to decide.\n",
				s->Symbol.Chr(), curList.Chr(), s->TotalRows
			);
			continue;
		}

		tPrintf
		(
			"\nPossible Norbert's Gambit: %s was bought in one currency and sold in\n"
			"another (%s) on the same day with matching share counts (%u row(s)).\n"
			"Such rows are usually the two legs of the gambit, which are not real\n"
			"trades for ShareSight.\n",
			s->Symbol.Chr(), curList.Chr(), s->TotalRows
		);

		char buf[32];
		tString choice;
		while (1)
		{
			tPrintf("Ignore %s? [y/N]: ", s->Symbol.Chr());
			fflush(stdout);
			if (!fgets(buf, (int)sizeof(buf), stdin))
			{
				AddReviewed(reviewed, s->Symbol);
				tPrintf("No input; keeping %s in the output (not asked again).\n", s->Symbol.Chr());
				break;
			}

			choice = buf;
			choice.Trim();
			if (choice.IsEqualCI("y") || choice.IsEqualCI("yes"))
			{
				AddIgnored(ignored, s->Symbol);
				tPrintf("Ignoring %s from the output.\n", s->Symbol.Chr());
				break;
			}
			if (choice.IsEmpty() || choice.IsEqualCI("n") || choice.IsEqualCI("no"))
			{
				AddReviewed(reviewed, s->Symbol);
				tPrintf("Keeping %s in the output (not asked again).\n", s->Symbol.Chr());
				break;
			}
			tPrintf("Please answer 'y' to ignore or 'n' to keep.\n");
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

	tString cur = currency;
	cur.Trim().ToUpper();

	// Match the exact (ticker, currency) pair; every recorded market carries a currency.
	for (MarketEntry* m = markets.First(); m; m = m->Next())
	{
		if (m->Ticker.IsEqualCI(key) && m->Currency.IsEqualCI(cur))
			return MarketCodeFromMIC(m->Mic);
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
	tList<MarketEntry>& markets, const tString& symbol, const tString& currency
)
{
	// The supported MICs in a stable order; the menu numbers follow this list. Desc is the long
	// market name shown in the menu so the user can tell the entries apart (eg. XNYS vs ARCX) --
	// kept in sync with the table in Data/Readme.txt.
	static const struct
	{
		const char* Mic;
		const char* Code;
		const char* Desc;
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
	tString cur = currency;
	cur.Trim().ToUpper();
	key += ':';
	key += cur;

	while (1)
	{
		tPrintf("\nWhich market did %s trade on?\n", key.Chr());
		for (int i = 0; i < menuCount; i++)
			tPrintf("  %d. %s  (ShareSight: %-6s) -- %s\n", i + 1, menu[i].Mic, menu[i].Code, menu[i].Desc);

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
		entry->Ticker = symbol;
		entry->Ticker.Trim().ToUpper();
		entry->Currency = currency;
		entry->Currency.Trim().ToUpper();
		entry->Mic = menu[index - 1].Mic;
		markets.Append(entry);

		tPrintf("Recorded %s %s (ShareSight Market Code: %s).\n", key.Chr(), menu[index - 1].Mic, menu[index - 1].Code);
		tPrintf("Config file:\n%s\n", ConfigFile().Chr());

		return true;
	}

	return false;
}


int BMO2SS::DateToDayNum(const tString& date)
{
	// Parse "YYYY-MM-DD" into a Julian Day Number for arithmetic comparison.
	int year = 0, month = 0, day = 0;
	sscanf(date.Chr(), "%d-%d-%d", &year, &month, &day);

	int a = (14 - month) / 12;
	int y = year + 4800 - a;
	int m = month + 12 * a - 3;
	return day + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
}


// Compute the brokerage fee for every trade using the sequential order-closing algorithm.
//
// Transactions are processed from oldest to newest. Each transaction is submitted to every
// open order in succession. An order's submit function first checks whether the date has
// advanced enough to close (fee in range and day passed, or max lookahead exceeded). Only
// if the order stays open does it check whether the transaction matches and can be consumed.
// If no order consumes the transaction, a new order is opened for it.
//
// Returns true on success; false if the user chose to quit.
bool BMO2SS::ComputeBrokerages(tList<Trade>& trades, double feeThreshold, int lookaheadDays)
{
	// A tList of reconstructed orders. Open orders absorb matching trades; closed ones are kept (and skipped by
	// the scan) so that a single projection pass at the end can write each placed fee onto its carrier leg.
	tList<Order> orders;

	// Verify the data is in reverse-chronological order (newest first in the list, which is the Append order).
	// We process from oldest to newest, so the list must be non-increasing by date.
	{
		const Trade* a = trades.First();
		for (const Trade* b = (a ? a->Next() : nullptr); b; a = b, b = b->Next())
		{
			if (DateToDayNum(a->TradeDate) < DateToDayNum(b->TradeDate))
			{
				tPrintf("Error: transactions are not in chronological order.\n");
				tPrintf("Row %d (%s) is older than row %d (%s).\n",
					a->InRow, a->TradeDate.Chr(), b->InRow, b->TradeDate.Chr());
				tPrintf("The exported data must be sorted newest-first.\n");
				return false;
			}
		}
	}

	// Process from oldest to newest (the list is newest-first, so iterate Last() -> Prev()).
	for (Trade* trade = trades.Last(); trade; trade = trade->Prev())
	{
		int dayNum = DateToDayNum(trade->TradeDate);

		bool consumed = false;
		for (Order* order = orders.First(); order; order = order->Next())
		{
			if (order->IsClosed())
				continue;

			int span = dayNum - order->DayNum;
			if (span < 0) span = -span;

			// If the date has advanced and the fee is in range, close (do NOT consume).
			if (span > 0 && order->InRange(feeThreshold))
			{
				order->Finalize(true, feeThreshold);
				continue;
			}

			// Exceeded the max lookahead: must close (do NOT consume).
			if (span > lookaheadDays)
			{
				if (order->InRange(feeThreshold))
				{
					order->Finalize(true, feeThreshold);
					continue;
				}
				if (!CanPromptForMarket())
				{
					tPrintf("Non-interactive: %s %s order exceeded %d days; fee set to zero.\n",
						order->Direction, order->Ticker.Chr(), lookaheadDays);
					order->Finalize(false, feeThreshold);
					continue;
				}
				tPrintf("Order %s %s (row %d, %d trades) exceeded the %d-day lookahead with an out-of-range fee.\n",
					order->Direction, order->Ticker.Chr(),
					(order->FeeCarrier ? order->FeeCarrier->InRow : 0), order->TradeCount, lookaheadDays);
				tPrintf("Set the fee to zero, or quit? [z/Q]: ");
				fflush(stdout);
				char buf[32];
				if (!fgets(buf, (int)sizeof(buf), stdin))
				{
					order->Finalize(false, feeThreshold);
					continue;
				}
				tString choice = buf;
				choice.Trim();
				if (choice.IsEqualCI("q"))
				{
					tPrintf("Aborted: the output file was not written.\n");
					return false;
				}
				order->Finalize(false, feeThreshold);
				continue;
			}

			// Within window, fee not yet in range: check if this trade matches and can be absorbed.
			if (order->CanConsume(*trade))
			{
				order->Submit(*trade);
				consumed = true;
				break;
			}
		}

		if (!consumed)
		{
			Order* order = new Order();
			orders.Append(order);
			order->Open(*trade);
		}
	}

	// Final sweep: close any remaining open orders.
	for (Order* order = orders.First(); order; order = order->Next())
	{
		if (order->IsClosed())
			continue;

		if (order->InRange(feeThreshold))
		{
			order->Finalize(true, feeThreshold);
		}
		else
		{
			tPrintf("Warning: %s %s order (row %d, %d trades) has an out-of-range fee.\n",
				order->Direction, order->Ticker.Chr(),
				(order->FeeCarrier ? order->FeeCarrier->InRow : 0), order->TradeCount);
			if (!CanPromptForMarket())
			{
				tPrintf("Non-interactive: fee set to zero.\n");
				order->Finalize(false, feeThreshold);
				continue;
			}
			tPrintf("Set the fee to zero, or quit? [z/Q]: ");
			fflush(stdout);
			char buf[32];
			if (!fgets(buf, (int)sizeof(buf), stdin))
			{
				order->Finalize(false, feeThreshold);
				continue;
			}
			tString choice = buf;
			choice.Trim();
			if (choice.IsEqualCI("q"))
			{
				tPrintf("Aborted: the output file was not written.\n");
				return false;
			}
			order->Finalize(false, feeThreshold);
			tPrintf("Fee set to zero.\n");
		}
	}

	// Projection pass: each placed fee is written onto its carrier leg (the highest |Total Amount| leg, ties -> last).
	for (Order* order = orders.First(); order; order = order->Next())
	{
		if (order->Place && order->FeeCarrier)
			order->FeeCarrier->Brokerage = tsrPrintf("%.2f", order->PlacedFee);
	}

	return true;
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

	tString outputFile =	BMO2SS::ParamOutput ?
							BMO2SS::ParamOutput.Get() :
							BMO2SS::DefaultOutputName(inputFile);

	tPrintf
	(
		"BmoIL2ShareSight V%d.%d.%d\n",
		Bmoil2ShareSightVersion::Major, Bmoil2ShareSightVersion::Minor, Bmoil2ShareSightVersion::Revision
	);

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

	// Config: market codes (per ticker, or per ticker+currency), the fee-ordering options, ignored tickers, and kept
	// (reviewed) tickers -- all in a single object loaded from the file next to the executable (tScript s-expr format).
	// Market codes are added when the user answers the prompt for a pair that is not recorded yet; ignored/kept tickers
	// are recorded when the user answers the Norbert's Gambit prompt.
	BMO2SS::Configuration config;
	config.Load(BMO2SS::ConfigFile());

	// The run records market codes and ignored/reviewed tickers as it goes. Persist whatever was learned on every
	// exit path (a successful run or any of the early "return 1" below) rather than saving piecemeal during the
	// prompts: this guard saves once, on scope exit, and covers each early return above it.
	struct SaveConfigOnScopeExit
	{
		const BMO2SS::Configuration& config;
		const tString&              filename;

		~SaveConfigOnScopeExit()
		{
			if (!config.Save(filename))
				tPrintf("Warning: could not save the config file.\n");
		}
	} saveConfigOnExit{ config, BMO2SS::ConfigFile() };

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
	for (int row = 0; row < numRows; row++)
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

	// Prepare the output document with the ShareSight bulk-trades header.
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

	// Conversion pass 1: collect the Buy/Sell trades (multi-trade orders are reconstructed later for the fees).
	int converted = 0;
	int skipped = 0;
	int ignoredRows = 0;

	// The trades live on a tList (it owns the nodes, which are freed automatically when the list is destroyed).
	tList<BMO2SS::Trade> trades;

	for (int row = headerRow + 1; row < numRows; row++)
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
		if (BMO2SS::IsIgnored(symbol, config.Ignored))
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
		// A zero or blank total is fine: in a multi-trade order the sibling trades carry the order's total.
		bool totalValid = total.IsValid() && total.IsNumeric(true, true);

		double totalAbs = 0.0;
		if (totalValid)
		{
			totalAbs = total.GetAsDouble();
			if (totalAbs < 0.0)
				totalAbs = -totalAbs;
		}

		// Append a new trade to the list (the tList owns the node and frees it on destruction).
		BMO2SS::Trade* newTrade = new BMO2SS::Trade();
		trades.Append(newTrade);
		BMO2SS::Trade& trade = *newTrade;
		trade.InRow = row + 1;
		trade.TradeDate = tradeDate;
		trade.SettleDate = settleDate;
		trade.Symbol = symbol;
		trade.Currency = currency;
		trade.Quantity = quantity;
		trade.Price = price;
		trade.TransactionType = transactionType;
		trade.TotalAbs = totalAbs;
		trade.ZeroTotal = (trade.TotalAbs <= 0.005);
		trade.Gross = quantity.GetAsDouble() * price.GetAsDouble();
		if (trade.Gross < 0.0)
			trade.Gross = -trade.Gross;

		converted++;
	}

	// Norbert's Gambit: detect symbols bought in one currency and sold in another on the same day with
	// matching share counts, and ask whether to drop them. A "keep" is recorded so the same ticker is not asked
	// again. This runs before the market-code prompts so a dropped symbol is not asked about.
	{
		tList<BMO2SS::NorbertSuspect> suspects;
		BMO2SS::FindNorbertSuspects(trades, config.Ignored, config.Reviewed, suspects);
		BMO2SS::PromptForNorbert(config.Ignored, config.Reviewed, suspects);
	}

	// Market codes: one per (ticker, currency) pair, recorded in the config file. Ask once per
	// pair that is not recorded yet; each answer is saved to the config file for future runs.
	for (const BMO2SS::Trade* trade = trades.First(); trade; trade = trade->Next())
	{
		if (BMO2SS::IsIgnored(trade->Symbol, config.Ignored))
			continue;
		if (!BMO2SS::MarketCodeFor(config.Markets, trade->Symbol, trade->Currency).IsEmpty())
			continue;

		tString pair = trade->Symbol;
		if (!trade->Currency.IsEmpty())
			pair += tsrPrintf(" (%s)", trade->Currency.Chr());

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

		if (!BMO2SS::PromptForMarket(config.Markets, trade->Symbol, trade->Currency))
		{
			tPrintf("Error: aborted -- the output would carry %s rows without a Market Code.\n", pair.Chr());
			return 1;
		}
	}

	// Compute the fees (reconstructing multi-trade orders), then write the rows back in input order.
	if (!BMO2SS::ComputeBrokerages(trades, config.FeeThreshold, config.LookaheadDays))
	{
		return 1;
	}

	int outRow = 1;
	int written = 0;
	int droppedNorbert = 0;

	for (const BMO2SS::Trade* trade = trades.First(); trade; trade = trade->Next())
	{
		// A ticker the user just chose to ignore (a detected Norbert's Gambit) was collected before the prompt;
		// drop it from the output now.
		if (BMO2SS::IsIgnored(trade->Symbol, config.Ignored))
		{
			droppedNorbert++;
			continue;
		}

		tList<tStringItem> outRowCells;
		outRowCells.Append(new tStringItem(trade->TradeDate));
		outRowCells.Append(new tStringItem(trade->Symbol));
		tString market = BMO2SS::MarketCodeFor(config.Markets, trade->Symbol, trade->Currency);
		if (market.IsEmpty())
			tPrintf
			(
				"Warning: Row %d: no market code for %s; the Market Code cell is left blank.\n",
				trade->InRow, trade->Symbol.Chr()
			);
		outRowCells.Append(new tStringItem(market));
		outRowCells.Append(new tStringItem(trade->Quantity));
		outRowCells.Append(new tStringItem(trade->Price));
		outRowCells.Append(new tStringItem(trade->TransactionType));
		outRowCells.Append(new tStringItem());	// Exchange Rate
		outRowCells.Append(new tStringItem(trade->Brokerage));
		outRowCells.Append(new tStringItem());	// Brokerage Currency
		outRowCells.Append(new tStringItem());	// Comments

		if (!output.SetRow(outRowCells, outRow))
		{
			tPrintf("Error: Could not write row %d to the output document.\n", outRow);
			return 1;
		}

		outRow++;
		written++;
	}

	if (written == 0)
	{
		tPrintf("Warning: No Buy or Sell rows were found; the output file\n");
		tPrintf("will contain only the header.\n");
	}

	// Save the result.
	if (!output.SaveFile(outputFile))
	{
		tPrintf("Error: Could not save output CSV:\n");
		tPrintf("%s\n", outputFile.Chr());
		return 1;
	}

	tPrintf("Input   : %s\n", inputFile.Chr());
	tPrintf("Output  : %s\n", outputFile.Chr());

	if (droppedNorbert > 0)
		tPrintf("Info    : Ignored %d row(s) for the detected Norbert's Gambit ticker(s).\n", droppedNorbert);

	if (ignoredRows > 0)
		tPrintf("Info    : Ignored %d row(s) for tickers on the ignore list.\n", ignoredRows);

	if ((droppedNorbert > 0) || (ignoredRows > 0))
		tPrintf("Config  : %s\n", BMO2SS::ConfigFile().Chr());

	tPrintf("Success : Converted %d Buy/Sell row(s); %d row(s) skipped.\n", written, skipped);
	return 0;
}
