Read this file with tab size set to 4 so tables line up.

________________________________________________________________________________
TransactionHistoryTest_2026-01-01_To_2026-12-31.csv

This file is structurally the same as cvs transaction data exported from
Investorline (at least as of Oct 2026). The share-counts and dates and exchange
rates etc are all fictional. The tickers are real, the transactions are not.

________________________________________________________________________________
bulk_trades.csv

This is the format that ShareSight expects. It is a template downloaded from
them.

________________________________________________________________________________
ShareSight CSV Structure

Although the below specs indicate Market Code is optional, that is only the case
if Combined Code is present. One of the two needs to be present. This conversion
tool uses Market Code.


Compulsory Fields
-----------------

Column Header		Description									Accepted Values							Example

Trade Date			The date of the trade.						yyyy-mm-dd (must be in this order,		2005-05-22
																Use 4 digits for year)

Instrument Code		The instrument code (stock ticker,			code									TLS
					fund name, etc)

Quantity			The quantity of shares bought or sold		Integer (should be a positive value)	1000

Price				The price per share in the currency of		Decimal (up to 6dp)						12.123456
					the market.

Transaction Type	Whether the trade was a buy or sell.		BUY, SELL								BUY


Optional Fields
---------------

Column Header		Description									Accepted Values							Example			Notes

Combined Code		The Instrument Code and Market Code			InstrumentCode.MarketCode				TLS.ASX			Can be used instead of individual columns
					separated by a period.																				for Instrument Code and Market Code

Market Code			The market code.							NYSE, NASDAQ, TSX, TSXV, NEO			TSX

Exchange Rate		The exchange rate to be applied to the		Decimal (up to 6dp)						1.123456		If not specified, will default to the closing
					share price (and brokerage if																		exchange rate on the trade date.
					applicable) - quoted as the amount of
					foreign currency that equals $1 of
					local currency.

Brokerage			The brokerage fee paid in the Brokerage		Decimal (up to 2dp)						39.95			If not specified, will default to zero.
					Currency specified.

Brokerage Currency	The currency code that the brokerage was	ISO 4217 format							AUD				Optional, If not specified will default to the market currency.
					paid in. It must be either your local
					currency or the currency of the market.

Comments			Comments to be stored in the comments		text									Recommended by
					field for the transaction.															Market Analysis
																										newsletter

Columns Sharesight Does Not Need
--------------------------------

Instrument name – Sharesight data provide this information from the instruction and market codes.
The total amount of holding bought or sold – Sharesight calculates this by the share price and quantity.

________________________________________________________________________________
Market Identifier Code (MIC) to Sharesight's Market Code

XTSE -> TSX		Toronto Stock Exchange
XTSX -> TSXV	Toronto Venture Stock Exchange
NEOE -> NEO		CBOE-Canada/NEO Exchange
XNAS -> NASDAQ	The NASDAQ Exchange
XNYS -> NYSE	New York Stock Exchange.
ARCX -> NYSE	New York Stock Exchange Arca.
