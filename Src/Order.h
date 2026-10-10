// Order.h
//
// A reconstructed "order": not a real broker order and not a CSV row. It is a transient fee bucket the algorithm
// builds in memory, accumulating the trades that settle together until the fee "closes". It is never persisted; it
// exists only for the duration of ComputeBrokerages to work out the fee and the leg (the highest-|Total Amount|
// trade, ties -> last) that will carry it.
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
#include <cmath>
#include <Foundation/tString.h>
#include <Foundation/tList.h>
#include "Trade.h"
namespace BMO2SS
{


class Order : public tLink<Order>
{
public:
	tString Ticker;		// Upper-case symbol.
	tString Direction;	// "BUY" or "SELL".
	tString Currency;	// Upper-case currency.
	int DayNum;			// Day number of the order's opening date.
	int TradeCount;		// Number of legs absorbed.
	double SumTotal;	// Running SUM of |Total Amount| (feeds Fee()).
	double SumGross;	// Running SUM of |Quantity| * Price (feeds Fee()).
	double MaxTotal;	// Running MAX of |Total Amount| (carrier selection).
	Trade* FeeCarrier;	// The max-|Total Amount| leg (ties -> last) that will receive the fee.
	bool Closed;		// True once finalised (no longer absorbs trades).
	bool Place;			// True when the fee is in range and non-zero (the projection pass writes it).
	double PlacedFee;	// The rounded fee to place (meaningful only when Place is true).

	// Set the identity from the first leg and accumulate it.
	void Open(Trade& first);

	// Absorb one more leg: keep the running sums (fee) and the running max (carrier, ties -> last via >=).
	void Submit(Trade& t);

	// True when this trade matches the order (same ticker, direction and currency).
	bool CanConsume(const Trade& t) const;

	// The order's brokerage fee (BUY: sumTotal - sumGross; SELL: sumGross - sumTotal).
	double Fee() const;

	// True when the fee is within the acceptable band (approximately zero to feeThreshold).
	bool InRange(double feeThreshold) const;

	// True once the order has been finalised (it no longer absorbs trades).
	bool IsClosed() const			{ return Closed; }

	// Finalise the order (it will no longer absorb trades). When 'place' is true the fee is in range and we record
	// it (blanking it if it rounds to ~0); otherwise the fee is left blank.
	void Finalize(bool place, double feeThreshold);
};


}


// Implementation only below this line.


inline void BMO2SS::Order::Open(Trade& first)
{
	Ticker = first.Symbol;
	Ticker.ToUpper();
	Direction = first.TransactionType;
	Currency = first.Currency;
	Currency.ToUpper();
	DayNum = first.DayNum();
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


inline void BMO2SS::Order::Submit(Trade& t)
{
	SumTotal += t.TotalAbs;
	SumGross += t.GrossValue();
	TradeCount++;
	if (t.TotalAbs >= MaxTotal)
	{
		FeeCarrier = &t;
		MaxTotal = t.TotalAbs;
	}
}


inline bool BMO2SS::Order::CanConsume(const Trade& t) const
{
	return Ticker.IsEqualCI(t.Symbol) &&
		Direction == t.TransactionType &&
		Currency.IsEqualCI(t.Currency);
}


inline double BMO2SS::Order::Fee() const
{
	if (Direction[0] == 'S')
		return SumGross - SumTotal;

	return SumTotal - SumGross;
}


inline bool BMO2SS::Order::InRange(double feeThreshold) const
{
	return (Fee() >= -1.0) && (Fee() <= feeThreshold);
}


inline void BMO2SS::Order::Finalize(bool place, double feeThreshold)
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
