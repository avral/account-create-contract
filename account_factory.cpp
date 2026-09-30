#include <eosio/asset.hpp>
#include <eosio/crypto.hpp>
#include <eosio/eosio.hpp>
#include <eosio/singleton.hpp>

#include <cstring>
#include <string_view>

using namespace eosio;

// Creates `<nick>.ac` accounts, paid for by the deposit that asks for one.
//
// A transfer of a listed token with memo `<nick>.ac:<owner key>:<active key>`
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

std::vector<char> base58_decode(std::string_view text) {
   static constexpr std::string_view ALPHABET =
      "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

   std::vector<uint8_t> bytes;  // least significant first while decoding
   for (const char c : text) {
      const size_t digit = ALPHABET.find(c);
      check(digit != std::string_view::npos, "key is not valid base58");

      uint32_t carry = digit;
      for (auto& byte : bytes) {
         carry += uint32_t(byte) * 58;
         byte = carry & 0xff;
         carry >>= 8;
      }
      for (; carry > 0; carry >>= 8) bytes.push_back(carry & 0xff);
   }
   for (size_t i = 0; i < text.size() && text[i] == '1'; ++i) bytes.push_back(0);

   return std::vector<char>(bytes.rbegin(), bytes.rend());
}

// `PUB_<type>_<base58(data || ripemd160(data || type)[0..4])>`, types K1, R1 and WA.
// The checksum is what keeps a mistyped key from creating an account nobody can use.
public_key parse_key(std::string_view text) {
   check(text.size() > 7 && text.substr(0, 4) == "PUB_" && text[6] == '_',
         "key must be PUB_K1_, PUB_R1_ or PUB_WA_");
   const std::string_view type = text.substr(4, 2);
   const std::vector<char> raw = base58_decode(text.substr(7));
   check(raw.size() > 4, "key is too short");

   const std::vector<char> data(raw.begin(), raw.end() - 4);
   std::vector<char> hashed = data;
   hashed.insert(hashed.end(), type.begin(), type.end());
   const auto digest = ripemd160(hashed.data(), hashed.size()).extract_as_byte_array();
   check(std::memcmp(raw.data() + data.size(), digest.data(), 4) == 0, "key checksum does not match");

   if (type == "K1" || type == "R1") {
      check(data.size() == 33, "key has the wrong length");
      ecc_public_key point;
      std::copy(data.begin(), data.end(), point.begin());
      if (type == "K1") return public_key{std::in_place_index<0>, point};
      return public_key{std::in_place_index<1>, point};
   }

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

      const auto parts = split(memo, ':');
      check(parts.size() == 3, "memo must be <nick>.ac:<owner key>:<active key>");
      const name account = parse_account(parts[0]);
      check(!is_account(account), "account name is taken");

      create_account(account, parse_key(parts[1]), parse_key(parts[2]));

      const asset rest = quantity - price.fee;
      if (rest.amount > 0) {
         action(permission_level{get_self(), "active"_n}, token, "transfer"_n,
                std::make_tuple(get_self(), account, rest, std::string(WELCOME_MEMO)))
            .send();
      }
   }

private:
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
