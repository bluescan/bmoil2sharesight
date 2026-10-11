# bmoil2sharesight
BmoIL2ShareSight is a small command-line tool to convert Investorline CSV files to a format ShareSight can understand. Handles multi-trade orders by grouping them together to avoid negative fees.

## Introduction

Run the `bmoil2sharesight` executable from a command prompt or shell. It prints its name and version number:
* `BmoIL2ShareSight V0.1.0`

Usage:
```
bmoil2sharesight.exe <input.csv> <output.csv>
```

The input file is an Investorline transaction history export (CSV, sorted newest-first). The output file is a ShareSight-compatible CSV.

## Fee Grouping

Investorline exports do not contain order IDs or any way to definitively accociate which transactions belong to which orders. It is an unfortunate oversight (at least as of October, 2026). This is why BmoIL2ShareSight reconstructs the orders using heuristics -- it hasn't gotten it wrong yet, but if it did the fee amounts and trade details would still balance correctly at the macro-scale. No incorrect information is introduced.

BmoIL2ShareSight reconstructs broker fees that are embedded in the "Total Amount" column of an Investorline export. When an order is split across multiple rows (trades) — as happens with limit orders that fill in parts across multiple days — the fee must be assigned to the order as a whole rather than to each individual trade.

### Algorithm

Transactions are processed from **oldest to newest** (the CSV is sorted newest-first, so the array is iterated in reverse). Each transaction is **submitted** to every currently open order in succession. An order decides independently what to do:

1. **Close check (date-driven):** If the current date is *after* the order's opening date, and the orders's accumulated fee is in the valid range (`$0 < fee ≤ threshold`), the order **closes immediately**. The transaction is *not* consumed, even if it matches the order's ticker.
2. **Max-lookahead check:** If the date span exceeds the configured maximum (default 7 days), the order is **force-closed** regardless of the fee value.
3. **Consume:** Only if the order remains open *and* the transaction matches the order (same ticker, direction, and currency) is it absorbed into the order. Direction means _buy_ or _sell_.
4. **Open new order:** If no order consumes the transaction, a new order is opened for it.

After all transactions are processed, any remaining open orders are finalized: In-range fees are placed, out-of-range fees prompt the user to zero them or quit.

### Fee Calculation

- **BUY:** `fee = Σ|Total Amount| − Σ(|Quantity| × Price)`
- **SELL:** `fee = Σ(|Quantity| × Price) − Σ|Total Amount|`

A fee is "in range" when it is **greater than $0** (or a small epsilon) and **at most the fee threshold** (default $100). This handles both the zero-total trade case (where one row records no cost, and the companion row carries the combined cost plus fee) and multi-trade fills across multiple days.

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
