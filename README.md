# bmoil2sharesight
BmoIL2ShareSight is a small command-line tool to convert Investorline CSV files to a format ShareSight understands. 

## Introduction

The problem this tool solves is two-fold:

1. BMO Investorline 2.0 does not export an 'Order ID' or something similar when multiple transactions are executed for the same order. This happens quite often when you place a limit order and parts of it get filled at different times and for different prices.
2. A naive ShareSight import script will compute the transaction fees incorrectly due to where Investorline places the costs for each transaction that belongs to a single order.

It is worth noting that this is the state as of Oct 12, 2026. Both platforms are actively being improved/developed so these behaviours may change in the future.

This tool handles multi-trade orders by determining the order grouping to avoid negative fees. `bmoil2sharesight` is a command-line tool that may be run from a command prompt or shell. The simplest invocation is:

`bmoil2sharesight input_transactions_from_il.csv`

which will generate a similarly-named csv that may be imported to ShareSight.


Usage:
```
bmoil2sharesight.exe <input.csv> <output.csv>
```

The input file is an Investorline transaction history export. The output file is a ShareSight-compatible CSV. If you don't specify the output a reasonable name will be generated for you.

`bmoil2sharesight -h`

will print some help text.


## Fee Grouping

Investorline exports do not contain order IDs or any way to definitively identify trades that belong to a specific order (at least as of October, 2026). BmoIL2ShareSight reconstructs the orders using heuristics so it can assign a single correct fee to all trades of a single order -- it hasn't gotten it wrong yet, but if it did the fee amounts and trade details would still balance correctly at the macro-scale. No incorrect information is introduced.

Here is an example input csv file (amounts, dates, and tickers are fictional)

```
Transaction Type=ALL,Product Type=All,Symbol=,From=2026-01-01,To=2026-12-31,,,,,
Transaction Date,Settlement Date,Activity Description,Description,Symbol,Quantity,Price,Price Currency,Total Amount,Total Currency
----------------,---------------,--------------------,-----------,------,--------,-----,--------------,------------,--------------
2026-12-31,2026-11-17,Buy,Vanguard FTSE Emerging Markets ETF,VWO,43,58.90,USD,-2532.70,USD
2026-08-06,2026-08-07,Buy,State Street SPDR Portfolio S&P 500 High Dividend ETF,SPYD,203,41.20,USD,0,USD
2026-08-06,2026-08-07,Buy,State Street SPDR Portfolio S&P 500 High Dividend ETF,SPYD,189,41.20,USD,-16160.35,USD
2026-06-05,2026-06-08,Buy,iShares MSCI Belgium ETF,EWK,7,55.20,USD,-396.35,USD
```

As you can see 203 shares of SPYD at $41.20 does not cost $0.00. That would yield a _fee_ of $8363.60. The next line is equally incorrect but in the opposite direction: 189 * $41.20 is $7786.80, not $16169.35. So, free shares on the first, and a huge overcharge on the second. What's going on here is both of these transactions are from the same limit order. The _totals_ work out: $16160.35 - (203 * $41.20) - (189 * $41.20) = $9.95. That's the actual fee. Yay. Incidentally I didn't factor out the $41.20 because there's no reason these prices actually need to be identical. In practice they will be slightly different, but the export only contains 2 decimal places so you may not always 'see' it.

The output from `BmoIL2ShareSight` looks like this:

```
Trade Date,Instrument Code,Market Code,Quantity,Price,Transaction Type,Exchange Rate (optional),Brokerage (optional),Brokerage Currency (optional),Comments (optional)
2026-12-31,VWO,NYSE,43,58.9000,BUY,,0.00,,
2026-08-06,SPYD,NYSE,203,41.2000,BUY,,,,
2026-08-06,SPYD,NYSE,189,41.2000,BUY,,9.95,,
2026-06-05,EWK,NYSE,7,55.2000,BUY,,9.95,,
```

As you can see the VWO was no-fee and the other 2 orders have a $9.95 fee each. Note that the transactions themselves are _not_ combined as it would make confirming transactions in ShareSight arduous and you would need to compute a weighted average cost when the prices don't match. Note also that the fee was attributed to the 189 unit buy -- this is because the 189 unit trade had the largest cost associated with it in the original data. This is the _fee carrying trade_. It needs to go somewhere and this made the most sense (distributing the fee would be... wierd, even if correct).


### Algorithm

Transactions are processed from **oldest to newest** (the CSV is sorted newest-first, so the array is iterated in reverse). Each transaction is **submitted** to every currently open order in succession. An order decides independently what to do:

1. **Close check (date-driven):** If the current date is *after* the order's opening date, and the order's accumulated fee is in the valid range (`$0 < fee ≤ threshold`), the order **closes immediately**. The transaction is *not* consumed, even if it matches the order's ticker.
2. **Max-lookahead check:** If the date span exceeds the configured maximum (default 7 days), the order is **force-closed** regardless of the fee value.
3. **Consume:** Only if the order remains open *and* the transaction matches the order (same ticker, direction, and currency) is it absorbed into the order. Direction means _buy_ or _sell_.
4. **Open new order:** If no order consumes the transaction, a new order is opened for it.

After all transactions are processed, any remaining open orders are finalized: In-range fees are placed, out-of-range fees prompt the user to zero them or quit.

### Fee Calculation

- **BUY:** `fee = Σ|Total Amount| − Σ(|Quantity| × Price)`
- **SELL:** `fee = Σ(|Quantity| × Price) − Σ|Total Amount|`

A fee is "in range" when it is **at least $0** (or a small epsilon, allowing for display rounding) and **at most the fee threshold** (default $100). This handles both the zero-total trade case (where one row records no cost, and the companion row carries the combined cost plus fee) and multi-trade fills across multiple days. A fee that rounds to 0.00 is written to the output as 0.00.

### Configuration

The following values are read from (and written back to) the config file after each run, under the `[Options]` section:

| Key | Default | Description |
|-----|---------|-------------|
| `FeeThreshold` | `100` | Maximum acceptable fee per order. Fees above this are treated as suspicious and prompt the user. |
| `MaxLookaheadDays` | `7` | Maximum day span an order may cover before it is force-closed. |

## Market Codes

ShareSight requires a **Market Code** for each instrument. When BmoIL2ShareSight encounters a (ticker, currency) pair that has no market code recorded, it prompts the user with a numbered menu of known markets. For example,

```
Market code needed for: QYLD (USD)
  1) XTSE (Toronto Stock Exchange)
  2) NEOE (CBOE-Canada/NEO Exchange)
  3) XNAS (NASDAQ Exchange)
  4) XNYS (New York Stock Exchange)
  5) ARCX (New York Stock Exchange Arca)
  6) ...
Enter the market code or a number:
```

The user's choice is **remembered in the config file** under `[Markets]` and will not be asked again in subsequent runs. If the tool is running non-interactively (e.g. in a CI pipeline) and no market code is found, it errors out with instructions to provide one manually.

## Norbert's Gambit Detection

The tool detects the common "Norbert's Gambit" pattern — a SELL in one market and a corresponding BUY in another market for the same ticker on the same day. For example:

```
SELL  RY  NYSE  -15   (US market)
BUY   RY  TSX     15   (Canadian market)
```

These are recognised as a currency conversion vehicle and **both legs are excluded from the output** if the user chooses to add the ticker to the `[Ignored]` list in the config file. If you need a different set of ignored tickers, edit the `[Ignored]` section of the config file directly. The detection also works when the buy/sell pair (legs) are multi-trade transactions.

## Building
It's a CMake C++ project. Install CMake and a C++20 compiler (MSVC on Windows, or Clang/GCC on Linux). The Tacent library is fetched automatically at configure time, or discovered if it is already installed on the machine.

Windows:
* `mkdir build`
* `cd build`
* `cmake ..`
* `cmake --build . --config Release`

Linux:
* `mkdir build && cd build`
* `cmake ..`
* `make`

## Version
The version number is defined in a single place, `Src/Version.cmake.h`. That one file drives both the CMake project version and the value the tool prints at runtime, so changing the number there updates both.
