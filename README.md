# account_factory

Creates `<nick>.ac` accounts on Telos. The account is paid for by the transfer
that asks for it, so the factory cannot be farmed: an account costs a deposit.

Deployed on **`ac`** (Telos mainnet), the owner of the `.ac` suffix. Only `ac`
can create `*.ac` names, so no one outside can take a name while its deposit is
on the way.

## How an account is made

One transfer to `ac` of a listed token, with memo:

```
<nick>.ac:<owner key>:<active key>
tst1.ac:PUB_K1_4yTp9kp2JsTudNPiZsJwTEdtK4QqAceoKwwMNYNTx1cMpFTRgW:PUB_WA_2iWG1BdLwgE4wZb98RZYRJibRdx…
```

In the same transaction the contract sends:

```
eosio::newaccount    ac -> <nick>.ac   owner and active one key each, threshold 1
eosio::buyrambytes   ac pays, ram_bytes from config
eosio::delegatebw    ac -> <nick>.ac   net/cpu stake from config, not transferred
<token>::transfer    ac -> <nick>.ac   quantity - fee, memo "Welcome to Alcor"
```

RAM and stake come out of the system-token (TLOS) reserve on `ac`. The fee stays
on `ac` in the token it was paid in. The stake stays Alcor's and can be taken
back with `undelegatebw`.

### Memo

- **Name.** `<nick>.ac`: nick is `a-z1-5`, no dots; the whole name is at most 12
  characters, so a nick is 1–9.
- **Keys.** `PUB_K1_`, `PUB_R1_` or `PUB_WA_`, checksum verified, so a mistyped key
  is refused instead of creating an account nobody can use. The legacy `EOS…` form
  is refused. Alcor Signer sends the seed's K1 as owner and the passkey's WA as
  active.
- **Reserve.** A transfer with memo `topup` is accepted as a reserve refill and
  creates nothing.

### Digits-only memo, for exchanges

Many exchanges refuse `:`, `.` or `_` in a withdrawal memo, and some take digits
only. So the same request can also be written as exactly **108 digits**:

```
15686568569603096576 1016584697680901733823934875031010872679428697709427288375000512758614788643813616221731
└─ name, 20 digits ─┘└──────────────────────────── K1 key + checksum, 88 digits ────────────────────────────┘
```

(no space in the real memo)

- **Name.** The account name as its `uint64` value, zero-padded to 20 digits.
  `vasya.ac` is `15686568569603096576`.
- **Key.** One K1 key, used for both owner and active: its 33 bytes and the same
  4 checksum bytes `PUB_K1_` carries, as one big-endian number, zero-padded to
  88 digits. That number is exactly what `PUB_K1_`'s base58 spells, so the
  checksum still catches a mistyped digit. Signer then puts the passkey on active
  itself, with an `updateauth` signed by the owner key.

A memo made only of digits is read this way; anything else is read as the text
form. Build it with the reference script, which front ends mirror:

```bash
node tools/numeric-memo.mjs vasya.ac PUB_K1_4yTp9kp2JsTudNPiZsJwTEdtK4QqAceoKwwMNYNTx1cMpFTRgW
```

A refused request from an exchange fails the withdrawal itself, so the tokens
stay on the exchange. Nothing ever waits on `ac` for a second step.

### Who sends the transfer

Anyone. The contract only sees a transfer of a listed token, and does not care
where it came from. In practice it comes from:

- **the router**, which delivers a user's cross-chain deposit to `ac` with the
  memo;
- **the bridge** (`alcor-bridge`) directly. A deposit with a memo is parked, and
  its relayer pushes `forward`, which transfers to `ac` with the memo;
- **a plain wallet or exchange withdrawal** of a listed token, e.g. TLOS if it is
  listed.

## When it refuses

Every check fails the whole transfer, and the tokens never leave the sender:

| message | cause |
|---|---|
| `this token does not pay for accounts` | token contract + symbol not in `fees` |
| `token precision does not match its listing` | same symbol, different precision |
| `deposit is below the account fee` | quantity < fee |
| `memo must be <nick>.ac:<owner key>:<active key>` | empty memo, or not three parts |
| `account name must end with .ac` | other suffix, or no suffix |
| `account name is longer than 12 characters` | nick of 10+ characters |
| `nick cannot contain a dot` | `a.b.ac` |
| `character is not in allowed character set for names` | uppercase, `0`, `6-9`, other characters |
| `account name is not in canonical form` | a name that does not round-trip |
| `key must be PUB_K1_, PUB_R1_ or PUB_WA_` | unknown prefix, legacy `EOS…` |
| `key is not valid base58` / `key is too short` / `key has the wrong length` | malformed key |
| `key checksum does not match` | mistyped key |
| `webauthn key has trailing bytes` / `webauthn key has no rpid` | malformed WA key |
| `numeric memo must be 108 digits` | a digits-only memo of any other length |
| `account number is out of range` | the name part is above `UINT64_MAX` |
| `account number is not a canonical name` | the name part does not round-trip as a name |
| `key is not a number` / `key number is out of range` | the key part is not a 37-byte number |
| `account name is taken` | the name already exists |
| `account factory is not configured` | `setconfig` never called |

For a bridge deposit a refusal fails `forward`: the deposit stays parked, and an
hour later the bridge `bounce`s it back to its `refundTo` on the source chain.
The user then releases it there and pays that chain's gas. A taken name is
refused too, not given a fallback name. So the front end checks all of the
above before the deposit is sent: the name is free (`get_account`), the keys
build, the token is listed and the amount covers the fee.

## Tables

| table | scope | row |
|---|---|---|
| `fees` | token contract | `fee`: what an account costs in that token |
| `config` | `ac` | `ram_bytes`, `net_stake`, `cpu_stake` given to each account |

`fees` is scoped by the token's contract. The same symbol issued by any other
contract pays for nothing, so a fake token cannot buy accounts.

## Actions

All need `ac@active`.

| action | |
|---|---|
| `setconfig(ram_bytes, net_stake, cpu_stake)` | stakes must be in the system token; `ram_bytes` > 0 |
| `setfee(token_contract, fee)` | list a token or change its fee; fee must be positive |
| `rmfee(token_contract, sym)` | unlist a token |

The fee has to be positive: a zero fee would let anyone drain the reserve for
dust. Collected fees and the reserve are ordinary balances of `ac`, moved with
ordinary transfers.

## Cost of an account

Measured on the first accounts made (2026-09-30):

| | |
|---|---|
| RAM bought, 4096 bytes | 0.2166 TLOS (incl. ram fee) |
| stake, 0.1 NET + 0.5 CPU | 0.6 TLOS, still Alcor's |
| `ac`'s own RAM for the new account's balance row | ~240 bytes |
| RAM an account with a WA active key uses | ~3018 bytes |

Telos adds about 1.4 KB on top of the RAM bought (`tst1.ac`: quota 5493 for
4096 bought), so 4096 leaves about 2.4 KB for the user's own rows. Fees are set
by the admin, at roughly cost + 50%.

## Live on Telos mainnet

| | |
|---|---|
| account | `ac` (won at auction by `avraldigital`, keys `EOS4yTp9…` + `ac@eosio.code` on active) |
| deployed | 2026-09-30, tx `177bd2a2f6bb8d9bc0c320c4afc6d42f9b7208a4e452591542f0f0ac62f8334e` |
| digits-only memo | 2026-10-01, tx `dc0a190108bf876e6ed9e45134c965619723972c961562c6e2bc9ce32c423ab1`, code hash `d04ad9b0…5b1f` |
| config | `4096`, `0.1000 TLOS`, `0.5000 TLOS` |
| fees | `wrap.alcor`: `0.015600 USDC`, `0.015600 USDT`, `2.50000000 WAX`, `0.00000580 ETH`; `eosio.token`: `0.9953 TLOS` — cost + 20% at TLOS $0.01558 (2026-09-30) |
| test accounts | `tst1.ac` (3 WAX, 2 forwarded), tx `d20d079a70e903c1c2f46faf19dbb97fa715bdc3689fb7d748edacf8c702f048` |
| | `acfactest.ac` (exactly the fee, nothing forwarded), tx `37fe41f5cf72a37eea723f95810af21e00d78e699b9e60b65f565935ca58e61e` |
| | `numtest.ac` (1.5 TLOS, digits-only memo), tx `cbe8bbc30f5f3e2f8f2bfbd1d61c4327656eb85cde1fcda20cf711ceea326439` |
| | `txttest.ac` (1.5 TLOS, text memo with a WA active), tx `39bb0f34331bd5f62151fd1bc6a2c77bce2698d8765940e6a2c3df9f2a18b279` |

Every refusal in the table above was also pushed on mainnet and refused with its
message. Not yet run end to end: a deposit arriving through the router or the
bridge, and a refused one coming back through `bounce`.

## Build

```bash
make    # build/account_factory.wasm + .abi, cdt-cpp 5
```

## Deploy and operate

```bash
# code (~320 KB of RAM on ac)
tcleos set contract ac build account_factory.wasm account_factory.abi -p ac@active
tcleos set account permission ac active --add-code -p ac@owner

# what an account gets, and what it costs
tcleos push action ac setconfig '[4096, "0.1000 TLOS", "0.5000 TLOS"]' -p ac@active
tcleos push action ac setfee '["wrap.alcor", "1.00000000 WAX"]' -p ac@active
tcleos get currency stats wrap.alcor USDC   # a token's precision, before setfee

# reserve
tcleos transfer <funder> ac "20.0000 TLOS" "topup"
tcleos get currency balance eosio.token ac
```

Watch the TLOS balance of `ac`: once the reserve runs out, every request is
refused at `buyrambytes`.
