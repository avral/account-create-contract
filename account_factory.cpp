#include <eosio/asset.hpp>
#include <eosio/crypto.hpp>
#include <eosio/eosio.hpp>
#include <eosio/singleton.hpp>

#include <algorithm>
#include <cstring>
#include <string_view>

using namespace eosio;

// Creates `<nick>.ac` accounts, paid for by the deposit that asks for one.
//
// A transfer of a listed token with memo `<nick>.ac:<owner key>:<active key>`,
// or the same request as 108 digits for exchanges that allow nothing else,
// creates the account, keeps the token's fee and sends the rest on to it. RAM and
// stake come out of this contract's own system-token reserve, refilled by a
// transfer with memo `topup`.
//
// Anything wrong with a request fails the whole transfer. A bridge deposit then
// stays parked, and the bridge sends it back to its depositor an hour later.

static constexpr name SYSTEM_ACCOUNT = "eosio"_n;
static constexpr std::string_view TOPUP_MEMO = "topup";
static constexpr std::string_view WELCOME_MEMO = "Welcome to Alcor";
static constexpr size_t MAX_NAME_LENGTH = 12;

// The digits-only memo, for exchanges that refuse anything else in one:
// the account name as a uint64, then one K1 key with its checksum, each
// zero-padded to a fixed width.
static constexpr size_t NUMERIC_NAME_DIGITS = 20;  // UINT64_MAX has 20 digits
static constexpr size_t NUMERIC_KEY_DIGITS = 88;   // 256^37 has 90, but a key starts 02 or 03
static constexpr size_t NUMERIC_KEY_BYTES = 37;    // 33 key bytes + 4 checksum bytes

struct key_weight {
   public_key key;
   uint16_t   weight;
   EOSLIB_SERIALIZE(key_weight, (key)(weight))
};

struct permission_level_weight {
   permission_level permission;
   uint16_t         weight;
   EOSLIB_SERIALIZE(permission_level_weight, (permission)(weight))
};

struct wait_weight {
   uint32_t wait_sec;
   uint16_t weight;
   EOSLIB_SERIALIZE(wait_weight, (wait_sec)(weight))
};

struct authority {
   uint32_t                             threshold;
   std::vector<key_weight>              keys;
   std::vector<permission_level_weight> accounts;
   std::vector<wait_weight>             waits;
   EOSLIB_SERIALIZE(authority, (threshold)(keys)(accounts)(waits))
};

// eosio.system's RAM market, read only to learn the system token.
struct exchange_state {
   asset supply;

   struct connector {
      asset  balance;
      double weight;
      EOSLIB_SERIALIZE(connector, (balance)(weight))
   };

   connector base;
   connector quote;

   uint64_t primary_key() const { return supply.symbol.raw(); }
};
using rammarket_table = multi_index<"rammarket"_n, exchange_state>;

namespace {

symbol core_symbol() {
   rammarket_table rammarket(SYSTEM_ACCOUNT, SYSTEM_ACCOUNT.value);
   check(rammarket.begin() != rammarket.end(), "system RAM market not found");
   return rammarket.begin()->quote.balance.symbol;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
   std::vector<std::string_view> parts;
   size_t start = 0;
   while (true) {
      const size_t end = text.find(separator, start);
      parts.push_back(text.substr(start, end - start));
      if (end == std::string_view::npos) return parts;
      start = end + 1;
   }
}

bool is_digits(std::string_view text) {
   return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Big-endian bytes of the number `text` spells in `alphabet`, with no leading zero bytes.
std::vector<char> decode_number(std::string_view text, std::string_view alphabet, const char* error) {
   std::vector<uint8_t> bytes;  // least significant first while decoding
   for (const char c : text) {
      const size_t digit = alphabet.find(c);
      check(digit != std::string_view::npos, error);

      uint32_t carry = digit;
      for (auto& byte : bytes) {
         carry += uint32_t(byte) * alphabet.size();
         byte = carry & 0xff;
         carry >>= 8;
      }
      for (; carry > 0; carry >>= 8) bytes.push_back(carry & 0xff);
   }

   return std::vector<char>(bytes.rbegin(), bytes.rend());
}

std::vector<char> base58_decode(std::string_view text) {
   // Each leading '1' stands for a zero byte, which the number itself drops.
   const size_t zeros = text.find_first_not_of('1');
   std::vector<char> bytes(zeros == std::string_view::npos ? text.size() : zeros, 0);

   const std::vector<char> number =
      decode_number(text, "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz", "key is not valid base58");
   bytes.insert(bytes.end(), number.begin(), number.end());
   return bytes;
}

uint64_t parse_uint64(std::string_view digits) {
   uint64_t value = 0;
   for (const char c : digits) {
      const uint64_t digit = c - '0';
      check(value <= (UINT64_MAX - digit) / 10, "account number is out of range");
      value = value * 10 + digit;
   }
   return value;
}

// `data || ripemd160(data || type)[0..4]` -> data. The checksum is what keeps a
// mistyped key from creating an account nobody can use.
std::vector<char> verify_checksum(const std::vector<char>& raw, std::string_view type) {
   check(raw.size() > 4, "key is too short");

   const std::vector<char> data(raw.begin(), raw.end() - 4);
   std::vector<char> hashed = data;
   hashed.insert(hashed.end(), type.begin(), type.end());
   const auto digest = ripemd160(hashed.data(), hashed.size()).extract_as_byte_array();
   check(std::memcmp(raw.data() + data.size(), digest.data(), 4) == 0, "key checksum does not match");
   return data;
}

ecc_public_key ecc_point(const std::vector<char>& data) {
   check(data.size() == 33, "key has the wrong length");
   ecc_public_key point;
   std::copy(data.begin(), data.end(), point.begin());
   return point;
}

// A K1 key written as one decimal number: its 33 bytes and 4 checksum bytes,
// big-endian, zero-padded to NUMERIC_KEY_DIGITS.
public_key parse_numeric_key(std::string_view digits) {
   const std::vector<char> number = decode_number(digits, "0123456789", "key is not a number");
   check(number.size() <= NUMERIC_KEY_BYTES, "key number is out of range");

   std::vector<char> raw(NUMERIC_KEY_BYTES - number.size(), 0);
   raw.insert(raw.end(), number.begin(), number.end());
   return public_key{std::in_place_index<0>, ecc_point(verify_checksum(raw, "K1"))};
}

// `PUB_<type>_<base58(data || ripemd160(data || type)[0..4])>`, types K1, R1 and WA.
public_key parse_key(std::string_view text) {
   check(text.size() > 7 && text.substr(0, 4) == "PUB_" && text[6] == '_',
         "key must be PUB_K1_, PUB_R1_ or PUB_WA_");
   const std::string_view type = text.substr(4, 2);
   const std::vector<char> data = verify_checksum(base58_decode(text.substr(7)), type);

   if (type == "K1") return public_key{std::in_place_index<0>, ecc_point(data)};
   if (type == "R1") return public_key{std::in_place_index<1>, ecc_point(data)};

   check(type == "WA", "key must be PUB_K1_, PUB_R1_ or PUB_WA_");
   datastream<const char*> ds(data.data(), data.size());
   webauthn_public_key key;
   ds >> key;
   check(ds.remaining() == 0, "webauthn key has trailing bytes");
   check(!key.rpid.empty(), "webauthn key has no rpid");
   return public_key{std::in_place_index<2>, key};
}

authority single_key(const public_key& key) {
   return authority{.threshold = 1, .keys = {{.key = key, .weight = 1}}, .accounts = {}, .waits = {}};
}

}  // namespace

class [[eosio::contract("account_factory")]] account_factory : public contract {
public:
   using contract::contract;

   // What an account costs in each accepted token. Scoped by the token's
   // contract, so the same symbol issued by anyone else pays for nothing.
   struct [[eosio::table]] fee_row {
      asset fee;
      uint64_t primary_key() const { return fee.symbol.code().raw(); }
   };
   using fees_table = multi_index<"fees"_n, fee_row>;

   // What every new account is given from the reserve.
   struct [[eosio::table]] config_row {
      uint32_t ram_bytes;
      asset    net_stake;
      asset    cpu_stake;
   };
   using config_singleton = singleton<"config"_n, config_row>;

   [[eosio::action]] void setconfig(uint32_t ram_bytes, const asset& net_stake, const asset& cpu_stake) {
      require_auth(get_self());
      check(ram_bytes > 0, "ram_bytes must be positive");

      const symbol core = core_symbol();
      check(net_stake.symbol == core && cpu_stake.symbol == core, "stake must be in the system token");
      check(net_stake.amount >= 0 && cpu_stake.amount >= 0, "stake cannot be negative");

      config_singleton(get_self(), get_self().value)
         .set(config_row{.ram_bytes = ram_bytes, .net_stake = net_stake, .cpu_stake = cpu_stake}, get_self());
   }

   [[eosio::action]] void setfee(name token_contract, const asset& fee) {
      require_auth(get_self());
      check(is_account(token_contract), "token contract does not exist");
      check(fee.is_valid() && fee.amount > 0, "fee must be positive");

      fees_table fees(get_self(), token_contract.value);
      const auto it = fees.find(fee.symbol.code().raw());
      if (it == fees.end()) {
         fees.emplace(get_self(), [&](auto& row) { row.fee = fee; });
      } else {
         fees.modify(it, same_payer, [&](auto& row) { row.fee = fee; });
      }
   }

   [[eosio::action]] void rmfee(name token_contract, const symbol_code& sym) {
      require_auth(get_self());
      fees_table fees(get_self(), token_contract.value);
      fees.erase(fees.require_find(sym.raw(), "token is not listed"));
   }

   [[eosio::on_notify("*::transfer")]]
   void on_transfer(name from, name to, const asset& quantity, const std::string& memo) {
      if (from == get_self() || to != get_self()) return;
      if (memo == TOPUP_MEMO) return;

      const name token = get_first_receiver();
      fees_table fees(get_self(), token.value);
      const auto& price = fees.get(quantity.symbol.code().raw(), "this token does not pay for accounts");
      check(price.fee.symbol == quantity.symbol, "token precision does not match its listing");
      check(quantity >= price.fee, "deposit is below the account fee");

      const account_request request = is_digits(memo) ? parse_numeric_request(memo) : parse_text_request(memo);
      check(!is_account(request.account), "account name is taken");

      create_account(request.account, request.owner, request.active);

      const asset rest = quantity - price.fee;
      if (rest.amount > 0) {
         action(permission_level{get_self(), "active"_n}, token, "transfer"_n,
                std::make_tuple(get_self(), request.account, rest, std::string(WELCOME_MEMO)))
            .send();
      }
   }

private:
   struct account_request {
      name       account;
      public_key owner;
      public_key active;
   };

   // `<nick>.ac:<owner key>:<active key>`
   account_request parse_text_request(std::string_view memo) const {
      const auto parts = split(memo, ':');
      check(parts.size() == 3, "memo must be <nick>.ac:<owner key>:<active key>");
      return {parse_account(parts[0]), parse_key(parts[1]), parse_key(parts[2])};
   }

   // 108 digits: the account name as a uint64, then one K1 key for both owner and active.
   account_request parse_numeric_request(std::string_view memo) const {
      check(memo.size() == NUMERIC_NAME_DIGITS + NUMERIC_KEY_DIGITS, "numeric memo must be 108 digits");

      const uint64_t value = parse_uint64(memo.substr(0, NUMERIC_NAME_DIGITS));
      const name account = parse_account(name(value).to_string());
      check(account.value == value, "account number is not a canonical name");

      const public_key key = parse_numeric_key(memo.substr(NUMERIC_NAME_DIGITS));
      return {account, key, key};
   }

   // `<nick>.ac`: a single dot, then the suffix this contract owns.
   name parse_account(std::string_view text) const {
      const std::string suffix = "." + get_self().to_string();
      check(text.size() > suffix.size() && text.substr(text.size() - suffix.size()) == suffix,
            "account name must end with " + suffix);
      check(text.size() <= MAX_NAME_LENGTH, "account name is longer than 12 characters");
      check(text.substr(0, text.size() - suffix.size()).find('.') == std::string_view::npos,
            "nick cannot contain a dot");

      const name account(text);  // refuses characters outside a-z, 1-5 and '.'
      check(account.to_string() == text, "account name is not in canonical form");
      return account;
   }

   void create_account(name account, const public_key& owner, const public_key& active) {
      const config_singleton config(get_self(), get_self().value);
      check(config.exists(), "account factory is not configured");
      const config_row cfg = config.get();
      const permission_level self{get_self(), "active"_n};

      action(self, SYSTEM_ACCOUNT, "newaccount"_n,
             std::make_tuple(get_self(), account, single_key(owner), single_key(active)))
         .send();
      action(self, SYSTEM_ACCOUNT, "buyrambytes"_n, std::make_tuple(get_self(), account, cfg.ram_bytes)).send();

      if (cfg.net_stake.amount > 0 || cfg.cpu_stake.amount > 0) {
         action(self, SYSTEM_ACCOUNT, "delegatebw"_n,
                std::make_tuple(get_self(), account, cfg.net_stake, cfg.cpu_stake, false))
            .send();
      }
   }
};
