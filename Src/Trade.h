// Trade.h
//
// One trade: a single row (a fill) of the Investorline input, kept in input order. Several trades of the same
// (ticker, currency) can belong to one order; the fee is computed over the whole order.
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


// Convert an ISO date string ("YYYY-MM-DD") to a day number for comparison (defined in BmoIL2ShareSight.cpp).
int DateToDayNum(const tString& date);


// One trade: a single row (a fill) of the input, kept in input order. The sign of Quantity distinguishes the
// direction (a Sell is negative).
struct Trade : tLink<Trade>
{
	// Build a trade from one validated input row.
	Trade
	(
		int inRow,
		const tString& tradeDate,
		const tString& settleDate,
		const tString& symbol,
		const tString& currency,
		int quantity,
		double price,
		const char* transactionType,
		double totalAbs
	);

	int         InRow;			// 1-based row in the input file (for warnings).
	tString     TradeDate;		// Trade date, ISO 8601 "YYYY-MM-DD".
	tString     SettleDate;		// Settlement date, ISO 8601 "YYYY-MM-DD".
	tString     Symbol;			// Instrument code (ticker).
	tString     Currency;		// Price currency of the trade (part of the (ticker, currency) market key).
	int         Quantity;		// Signed whole number of units; a Sell is negative.
	double      Price;			// Price per unit, in the trade's currency.
	const char* TransactionType;	// "BUY" or "SELL".
	double      TotalAbs;		// |Total Amount| (0 when the cell is empty).
	tString     Brokerage;		// Computed fee; empty means the cell is left blank.

	// |Quantity| (the sign is irrelevant to the fee maths).
	int AbsQuantity() const;

	// |Quantity| * Price (equivalent to the old cached Gross), computed on demand.
	double GrossValue() const;

	// The day number of the trade date, for chronological comparison.
	int DayNum() const
	{
		return DateToDayNum(TradeDate);
	}
};


}


// Implementation only below this line.


inline BMO2SS::Trade::Trade
(
	int inRow,
	const tString& tradeDate,
	const tString& settleDate,
	const tString& symbol,
	const tString& currency,
	int quantity,
	double price,
	const char* transactionType,
	double totalAbs
)
	: InRow(inRow),
	TradeDate(tradeDate),
	SettleDate(settleDate),
	Symbol(symbol),
	Currency(currency),
	Quantity(quantity),
	Price(price),
	TransactionType(transactionType),
	TotalAbs(totalAbs)
{
}


inline int BMO2SS::Trade::AbsQuantity() const
{
	return (Quantity < 0) ? -Quantity : Quantity;
}


inline double BMO2SS::Trade::GrossValue() const
{
	double g = Quantity * Price;
	return (g < 0.0) ? -g : g;
}
