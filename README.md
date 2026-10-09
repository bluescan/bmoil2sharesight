# bmoil2sharesight
BmoIL2ShareSight is a small command-line tool to convert Investorline CSV files to a format ShareSight can understand. Handles multi-leg trades by grouping them together to avoid negative fees.

## Introduction

Run the `bmoil2sharesight` executable from a command prompt or shell. It prints
its name and version number:
* `BmoIL2ShareSight V0.1.0`

Usage:
```
bmoil2sharesight.exe <input.csv> <output.csv>
```

The input file is an Investorline transaction history export (CSV, sorted
newest-first). The output file is a ShareSight-compatible CSV.

## Fee Grouping

BmoIL2ShareSight reconstructs broker fees that are embedded in the
"Total Amount" column of an Investorline export. When a trade is split
across multiple rows (legs) — as happens with limit orders that fill in
parts across multiple days — the fee must be assigned to the group as a
whole rather than to each individual leg.

### Algorithm

Transactions are processed from **oldest to newest** (the CSV is sorted
newest-first, so the array is iterated in reverse). Each transaction is
**submitted** to every currently open group in succession. A group
decides independently what to do:

1. **Close check (date-driven):** If the current date is *after* the
   group's opening date, and the group's accumulated fee is in the
   valid range (`$0 < fee ≤ threshold`), the group **closes
   immediately**. The transaction is *not* consumed, even if it matches
   the group's ticker.
2. **Max-lookahead check:** If the date span exceeds the configured
   maximum (default 7 days), the group is **force-closed** regardless of
   the fee value.
3. **Consume:** Only if the group remains open *and* the transaction
   matches the group (same ticker, direction, and currency) is it
   absorbed into the group.
4. **Open new group:** If no group consumes the transaction, a new
   group is opened for it.

After all transactions are processed, any remaining open groups are
finalized: in-range fees are placed, out-of-range fees prompt the user
to zero them or quit.

### Fee Calculation

- **BUY:** `fee = Σ|Total Amount| − Σ(|Quantity| × Price)`
- **SELL:** `fee = Σ(|Quantity| × Price) − Σ|Total Amount|`

A fee is "in range" when it is **greater than $0** (or a small
epsilon) and **at most the fee threshold** (default $100). This
handles both the zero-total leg case (where one row records no cost,
and the companion row carries the combined cost plus fee) and
multi-leg fills across multiple days.

### Configuration

The following values are read from (and written back to) the config
file after each run, under the `[Options]` section:

| Key | Default | Description |
|-----|---------|-------------|
| `feeThreshold` | `100` | Maximum acceptable fee per group. Fees above this are treated as suspicious and prompt the user. |
| `maxLookaheadDays` | `7` | Maximum day span a group may cover before it is force-closed. |

## Market Codes

ShareSight requires a **Market Code** for each instrument. When BmoIL2ShareSight
encounters a (ticker, currency) pair that has no market code recorded, it
prompts the user with a numbered menu of known markets:

```
Market code needed for: QYLD (USD)
  1) NASDAQ
  2) NYSE
  3) TSXV
Enter the market code or a number:
```

The user's choice is **remembered in the config file** under
`[Markets]` and will not be asked again in subsequent runs. If the
tool is running non-interactively (e.g. in a CI pipeline) and no market
code is found, it errors out with instructions to provide one manually.

## Norbert's Gambit Detection

The tool detects the common "Norbert's Gambit" pattern — a SELL in one
market and a corresponding BUY in another market for the same ticker on
the same day. For example:

```
SELL  RY  NYSE  -15   (US market)
BUY   RY  TSX     15   (Canadian market)
```

These are recognised as a currency conversion vehicle and **both legs
are excluded from the output** if the user chooses to add the ticker to the
`[Ignored]` list in the config file. If you need a different set of ignored
tickers, edit the `[Ignored]` section of the config file directly. The
detection also works when the buy/sell pair are multi-leg transactions.

## Building
It's a CMake C++ project. Install CMake and a C++20 compiler (MSVC on Windows,
or Clang/GCC on Linux). The Tacent library is fetched automatically at configure
time, or discovered if it is already installed on the machine.

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
The version number is defined in a single place, `Src/Version.cmake.h`. That one
file drives both the CMake project version and the value the tool prints at
runtime, so changing the number there updates both.
