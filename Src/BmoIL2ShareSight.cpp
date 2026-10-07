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
// bmoil2sharesight.cfg config file (next to the executable): one "TICKER=MIC" or "TICKER.CURRENCY=MIC" line per pair.
// The MIC is a standard ISO 10383 market identifier code (XNYS, ARCX, XNAS, XTSE, XTSX, NEOE); the matching ShareSight
// Market Code (NYSE, NASDAQ, TSX, TSXV, NEO) is what gets saved in the output CSV. When a pair is not recorded, the
// tool lists the supported MICs and asks the user to pick one by number, then saves the choice to the config file so it
// is not asked again.
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
// Ignored tickers (eg. Norbert's Gambit vehicles): the --ignore option drops a symbol entirely and appends it as a
// plain line to the bmoil2sharesight.cfg config file (next to the executable, alongside the market codes) so it is
// remembered for future runs.
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
	tCmdLine::tOption OptionIgnore("Ignore a ticker symbol entirely (eg. a Norbert's Gambit vehicle like DXYZ). Repeatable; the ticker is also saved to the bmoil2sharesight.cfg config file next to the executable so it is remembered for future runs.", "ignore", 'g', 1);
	tCmdLine::tOption OptionHelp("Display help.", "help", 'h');

	// One leg of a (possibly multi-leg) buy/sell trade, kept in input order.
	struct TradeLeg
	{
		int inRow;                    // 1-based row in the input file (for warnings)
		tString tradeDate;
		tString settleDate;
		tString symbol;
		tString currency;            // the Price Currency of the leg (part of the (ticker, currency) market key)
		tString quantity;
		tString price;
		const char* transactionType;  // "BUY" or "SELL"
		bool zeroTotal;               // Total Amount cell was empty or 0
		double totalAbs;              // |Total Amount| (0 when the cell is empty)
		double gross;                 // |Quantity| * Price
		tString brokerage;            // computed fee; empty == left blank
		tString groupKey;             // activity|SYMBOL|tradeDate|settleDate|CURRENCY (upper-cased)
	};

	// Replaces the file extension of the supplied path with "_sharesight.csv".
	tString DefaultOutputName(const tString& inputName);

	// The config file remembers the market codes and the ignored tickers across runs. It lives next to
	// the executable:
	//   <exe-dir>/bmoil2sharesight.cfg
	// Market-code lines are TICKER=MIC or TICKER.CURRENCY=MIC (ISO 10383 MICs); every other non-comment
	// line is an ignored ticker. Blank lines and lines starting with '#' are ignored.
	tString ConfigFile();

	// True if the symbol is on the ignore list (case-insensitive).
	bool IsIgnored(const tString& symbol, tList<tStringItem>& ignored);

	// Load the market codes and the ignore list from the config file, if the file exists.
	void LoadConfig(tList<tStringItem>& markets, tList<tStringItem>& ignored);

	// Add a ticker from the --ignore option; returns true if it was new (so the config file must be saved).
	bool AddIgnored(tList<tStringItem>& ignored, const tString& ticker);

	// Persist the market codes and the ignore list to the config file.
	bool SaveConfig(tList<tStringItem>& markets, tList<tStringItem>& ignored);

	// Convert a MIC to the ShareSight Market Code that is saved in the output CSV. Returns an empty
	// string when the MIC is not one of the supported codes (see the definition below).
	tString MarketCodeFromMIC(const tString& mic);

	// The ShareSight Market Code recorded for a (ticker, currency) pair; falls back from
	// TICKER.CURRENCY to TICKER. Empty when the pair is not recorded.
	tString MarketCodeFor(tList<tStringItem>& markets, const tString& symbol, const tString& currency);

	// Look up the (upper-cased) key in the market-code list; on success fills in its MIC.
	bool FindMarket(tList<tStringItem>& markets, const tString& key, tString& micOut);

	// True when a market code can be asked for: stdin is an interactive terminal, or piped input that
	// already has data (an empty pipe cannot answer, so the prompt would only block).
	bool CanPromptForMarket();

	// Ask (numbered menu) which market a (ticker, currency) pair traded on, record the choice in the
	// market list and save the config file. Returns false to abort the run.
	bool PromptForMarket(tList<tStringItem>& markets, tList<tStringItem>& ignored,
						 const tString& symbol, const tString& currency);

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


void BMO2SS::LoadConfig(tList<tStringItem>& markets, tList<tStringItem>& ignored)
{
	tString text;
	if (!tSystem::tLoadFile(ConfigFile(), text))
	{
		return;
	}
	tList<tStringItem> lines;
	tStd::tExplode(lines, text, '\n');
	for (tStringItem* item = lines.First(); item; item = item->Next())
	{
		tString line = *item;
		line.Trim();
		if (line.IsEmpty() || line[0] == '#')
			continue;

		// A line carrying '=' is a market-code entry (TICKER=MIC or TICKER.CURRENCY=MIC); anything
		// else is an ignored ticker.
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
			ignored.Append(new tStringItem(line));
			continue;
		}

		// Split "TICKER(.CURRENCY)=MIC". ExtractLeft('=') returns the left part (and leaves the right part in the
		// string); Right('=') is non-destructive. Use the returned left part as the key.
		tString keySrc = line;
		tString micSrc = line;
		tString key = keySrc.ExtractLeft('=');
		key.Trim().ToUpper();
		tString mic = micSrc.Right('=');
		mic.Trim().ToUpper();
		if (key.IsEmpty() || mic.IsEmpty() || MarketCodeFromMIC(mic).IsEmpty())
		{
			tPrintf
			(
				"Warning: ignoring the malformed or unsupported market entry '%s' in %s.\n",
				line.Chr(), ConfigFile().Chr()
			);
			continue;
		}

		tString entry = key;
		entry += char('=');
		entry += mic;
		markets.Append(new tStringItem(entry));
	}
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


bool BMO2SS::SaveConfig(tList<tStringItem>& markets, tList<tStringItem>& ignored)
{
	tString cfgFile = ConfigFile();
	if (tSystem::tFileExists(cfgFile) && !tSystem::tDeleteFile(cfgFile))
		return false;

	tString text = "# BmoIL2ShareSight configuration.\n";
	text += "# Market codes: TICKER=MIC or TICKER.CURRENCY=MIC -- the MIC (eg. XNYS) is converted to the\n";
	text += "# ShareSight Market Code (eg. NYSE) that is saved in the output CSV.\n";
	for (tStringItem* item = markets.First(); item; item = item->Next())
	{
		text += *item;
		text += "\n";
	}
	if (ignored.First())
	{
		text += "# Ignored tickers (one per line).\n";
		for (tStringItem* item = ignored.First(); item; item = item->Next())
		{
			text += *item;
			text += "\n";
		}
	}
	return tSystem::tCreateFile(cfgFile, text);
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


bool BMO2SS::FindMarket(tList<tStringItem>& markets, const tString& key, tString& micOut)
{
	for (tStringItem* item = markets.First(); item; item = item->Next())
	{
		tString entryKey(*item);
		tString entry = entryKey.ExtractLeft('=');	// ExtractLeft returns the key (left of '=').
		entry.Trim().ToUpper();
		if (entry.IsEqualCI(key))
		{
			micOut = item->Right('=');
			return true;
		}
	}
	return false;
}


tString BMO2SS::MarketCodeFor(tList<tStringItem>& markets, const tString& symbol, const tString& currency)
{
	tString key = symbol;
	key.Trim().ToUpper();
	if (key.IsEmpty())
		return tString();

	// Prefer the (ticker, currency) pair; fall back to the ticker alone.
	if (!currency.IsEmpty())
	{
		tString full = key;
		full += char('.');
		tString cur = currency;
		cur.Trim().ToUpper();
		full += cur;

		tString mic;
		if (FindMarket(markets, full, mic))
			return MarketCodeFromMIC(mic);
	}

	tString mic;
	if (FindMarket(markets, key, mic))
		return MarketCodeFromMIC(mic);

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
	tList<tStringItem>& markets, tList<tStringItem>& ignored,
	const tString& symbol, const tString& currency
)
{
	// The supported MICs in a stable order; the menu numbers follow this list.
	static const struct
	{
		const char* mic;
		const char* code;
	} menu[] =
	{
		{ "XTSE", "TSX" },
		{ "XTSX", "TSXV" },
		{ "NEOE", "NEO" },
		{ "XNAS", "NASDAQ" },
		{ "XNYS", "NYSE" },
		{ "ARCX", "NYSE" },
	};

	static const int menuCount = (int)(sizeof(menu) / sizeof(menu[0]));

	tString key = symbol;
	key.Trim().ToUpper();
	if (!currency.IsEmpty())
	{
		key += char('.');
		tString cur = currency;
		cur.Trim().ToUpper();
		key += cur;
	}

	while (1)
	{
		tPrintf("\nWhich market did %s trade on?\n", key.Chr());
		for (int i = 0; i < menuCount; ++i)
			tPrintf("  %d. %s  (ShareSight: %s)\n", i + 1, menu[i].mic, menu[i].code);

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

		tString entry = key;
		entry += char('=');
		entry += tString(menu[index - 1].mic);
		markets.Append(new tStringItem(entry));

		if (SaveConfig(markets, ignored))
			tPrintf
			(
				"Saved %s=%s (ShareSight Market Code: %s) to %s.\n",
				key.Chr(), menu[index - 1].mic, menu[index - 1].code, ConfigFile().Chr()
			);
		else
			tPrintf
			(
				"Warning: could not save the market code to %s (it will be needed again next run).\n",
				ConfigFile().Chr()
			);

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
		int firstZero   = -1;
		int lastLeg     = -1;
		int lastNonZero = -1;
		double sumTotal = 0.0;
		double sumGross = 0.0;
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
		tPrintf("Error: Input file does not exist: %s\n", inputFile.Chr());
		return 1;
	}

	tString outputFile = BMO2SS::ParamOutput ? BMO2SS::ParamOutput.Get()
														: BMO2SS::DefaultOutputName(inputFile);

	// Config: market codes (TICKER=CURRENCY=MIC pairs) and ignored tickers, remembered in a file next to the
	// executable. --ignore adds tickers; market codes are added when the user answers the prompt for a pair that is
	// not recorded yet.
	tList<tStringItem> marketMap;
	tList<tStringItem> ignoreList;
	BMO2SS::LoadConfig(marketMap, ignoreList);

	bool ignoreDirty = false;
	if (BMO2SS::OptionIgnore)
	{
		tCmdLine::tOption& opt = BMO2SS::OptionIgnore;
		for (int a = 0; a < opt.GetNumTotalArgs(); ++a)
		{
			if (BMO2SS::AddIgnored(ignoreList, opt.ArgN(a + 1)))
				ignoreDirty = true;
		}
	}

	if (ignoreDirty && !BMO2SS::SaveConfig(marketMap, ignoreList))
		tPrintf
		(
			"Warning: could not save the config to %s (the tickers are ignored for this run only).\n",
			BMO2SS::ConfigFile().Chr()
		);

	// Load the Investorline export.
	tSystem::tCSV input;
	if (!input.LoadFile(inputFile))
	{
		tPrintf("Error: Could not load input CSV: %s\n", inputFile.Chr());
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
		tPrintf("Warning: Could not find the 'Transaction Date' header row; assuming the first line.\n");
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

		tString settleDate = input.Get(row, 1);
		settleDate.Trim();
		tString activity = input.Get(row, 2);
		activity.Trim();
		tString symbol   = input.Get(row, 4);
		symbol.Trim();
		tString quantity = input.Get(row, 5);
		quantity.Trim();
		tString price    = input.Get(row, 6);
		price.Trim();
		tString currency = input.Get(row, 7);
		currency.Trim();
		tString total    = input.Get(row, 8);
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
			tPrintf("Warning: row %d: activity '%s' is not a Buy or Sell; skipped.\n", row + 1, activity.Chr());
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

	// ---- Market codes: one per (ticker, currency) pair, recorded in the config file. Ask once per
	// pair that is not recorded yet; each answer is saved to the config file for future runs.
	for (int i = 0; i < numLegs; i++)
	{
		const BMO2SS::TradeLeg& leg = legs[i];
		if (!BMO2SS::MarketCodeFor(marketMap, leg.symbol, leg.currency).IsEmpty())
			continue;

		tString pair = leg.symbol;
		if (!leg.currency.IsEmpty())
			pair += tsrPrintf(" (%s)", leg.currency.Chr());

		if (!BMO2SS::CanPromptForMarket())
		{
			tPrintf("Error: no market code is recorded for %s, and stdin is not interactive (no one is there to answer).\n",
					pair.Chr());
			tPrintf("Record it in %s as a 'TICKER=MIC' or 'TICKER.CURRENCY=MIC' line -- supported MICs: XTSE, XTSX, NEOE, XNAS, XNYS, ARCX (see Data/Readme.txt).\n",
					BMO2SS::ConfigFile().Chr());
			return 1;
		}

		if (!BMO2SS::PromptForMarket(marketMap, ignoreList, leg.symbol, leg.currency))
		{
			tPrintf("Error: aborted -- the output would carry %s rows without a Market Code.\n", pair.Chr());
			return 1;
		}
	}

	// Compute the fees (grouping multi-leg orders), then write the rows back in input order.
	BMO2SS::ComputeBrokerages(legs, numLegs);

	int outRow = 1;

	for (int i = 0; i < numLegs; i++)
	{
		const BMO2SS::TradeLeg& leg = legs[i];

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
	}

	BMO2SS::FreeLegs(legs, numLegs);

	if (converted == 0)
	{
		tPrintf("Warning: No Buy or Sell rows were found; the output file will contain only the header.\n");
	}

	// ---- Save the result.
	if (!output.SaveFile(outputFile))
	{
		tPrintf("Error: Could not save output CSV: %s\n", outputFile.Chr());
		return 1;
	}

	tPrintf
	(
		"BmoIL2ShareSight V%d.%d.%d\n",
		Bmoil2ShareSightVersion::Major, Bmoil2ShareSightVersion::Minor, Bmoil2ShareSightVersion::Revision
	);

	tPrintf
	(
		"Converted %d Buy/Sell row(s) from '%s' to '%s'. %d row(s) skipped.\n",
		converted, inputFile.Chr(), outputFile.Chr(), skipped
	);

	if (ignoredRows > 0)
		tPrintf
		(
			"Ignored %d row(s) for tickers on the ignore list (%s).\n",
			ignoredRows, BMO2SS::ConfigFile().Chr()
		);

	return 0;
}
