#include "airhodl.hpp"

namespace airhodl {

void airhodl::create( name   issuer,
                    asset  maximum_supply )
{
    require_auth( _self );

    auto sym = maximum_supply.symbol;
    eosio::check( sym.is_valid(), "invalid symbol name" );
    eosio::check( maximum_supply.is_valid(), "invalid supply");
    eosio::check( maximum_supply.amount > 0, "max-supply must be positive");

    stats statstable( _self, sym.code().raw() );
    auto existing = statstable.find( sym.code().raw() );
    eosio::check( existing == statstable.end(), "token with symbol already exists" );

    statstable.emplace( _self, [&]( auto& s ) {
       s.supply.symbol     = maximum_supply.symbol;
       s.forfeiture.symbol = maximum_supply.symbol;
       s.max_supply    = maximum_supply;
       s.issuer        = issuer;
       s.vesting_start = time_point();
       s.vesting_end   = time_point();
    });
}

void airhodl::activate( const symbol& symbol, time_point start, time_point end) {
   auto sym_code_raw = symbol.code().raw();

   stats statstable( _self, sym_code_raw );
   auto existing = statstable.find( sym_code_raw );
   eosio::check( existing != statstable.end(), "token with symbol does not exist, create token before activation" );
   const auto& st = *existing;

   require_auth( st.issuer );

   eosio::check(start > current_time_point(), "vesting start must be in the future");
   eosio::check(end > start, "vesting end must be later than vesting start");  

   statstable.modify( st, eosio::same_payer, [&]( auto& s ) {
      s.vesting_start = start;
      s.vesting_end   = end;
   });   
}


void airhodl::issue( name to, asset quantity, string memo )
{
    auto sym = quantity.symbol;
    eosio::check( sym.is_valid(), "invalid symbol name" );
    eosio::check( memo.size() <= 256, "memo has more than 256 bytes" );

    stats statstable( _self, sym.code().raw() );
    auto existing = statstable.find( sym.code().raw() );
    eosio::check( existing != statstable.end(), "token with symbol does not exist, create token before issue" );
    const auto& st = *existing;

    require_auth( st.issuer );
    eosio::check( quantity.is_valid(), "invalid quantity" );
    eosio::check( quantity.amount > 0, "must issue positive quantity" );

    eosio::check( quantity.symbol == st.supply.symbol, "symbol precision mismatch" );
    eosio::check( quantity.amount <= st.max_supply.amount - st.supply.amount, "quantity exceeds available supply");

    statstable.modify( st, eosio::same_payer, [&]( auto& s ) {
       s.supply += quantity;
    });

    add_balance( to, quantity, st.issuer );
}

void airhodl::grab( name owner, const symbol& symbol, name ram_payer )
{
   require_auth( ram_payer );
   require_recipient( owner );

   auto sym_code_raw = symbol.code().raw();

   accounts acnts( _self, owner.value );
   auto it = acnts.find( sym_code_raw );
   eosio::check(it != acnts.end(), "no balance to grab");
   eosio::check(it->claimed == false, "already grabbed");

   asset balance = it->balance;
   asset staked  = it->staked;
   asset allocation = it->allocation;
   acnts.erase(it);

   acnts.emplace( ram_payer, [&]( auto& a ){
      a.balance = balance;
      a.allocation = allocation;
      a.staked  = staked;
      a.claimed = true;
   });
}

void airhodl::withdraw( name owner, const symbol& symbol ) {   
   require_auth(owner);

   //Find token stats
   auto sym_code_raw = symbol.code().raw();

   stats statstable( _self, sym_code_raw );
   auto existing = statstable.find( sym_code_raw );
   eosio::check( existing != statstable.end(), "symbol does not exist for withdrawal" );
   const auto& st = *existing;

   //Check if vesting has started
   eosio::check(st.vesting_start > time_point(),"vesting has not started");

   //Find hodlaccts
   accounts from_acnts( _self, owner.value );
   const auto& from = from_acnts.get( sym_code_raw, "no balance object found" );
   
   //Ensure that stake is 0
   eosio::check(from.staked.amount == 0, "you must fully unstake to withdraw");

   //calculate vesting ratio
   
   auto time_elapsed = current_time_point() - st.vesting_start;
   auto vesting_duration = st.vesting_end - st.vesting_start;
   double vesting_ratio = double(time_elapsed.count()) / double(vesting_duration.count());

   //calculate vested_balance
   uint64_t balance_vested = static_cast<uint64_t>(vesting_ratio * double(from.allocation.amount));
   uint64_t balance_forfeited = from.allocation.amount - balance_vested;
   double   bonus_share = double(st.forfeiture.amount) * (double(from.allocation.amount) / double(st.supply.amount));
   uint64_t bonus_vested = static_cast<uint64_t>(vesting_ratio * bonus_share);
   asset    payout = asset(balance_vested + bonus_vested, st.supply.symbol);

   //update tables
   statstable.modify(st, eosio::same_payer, [&](auto &s) {
      //decrease supply by the amount paid out
      s.supply -= (asset(balance_vested,symbol) + asset(bonus_vested,symbol)); 
      //increase forfeight hodl by forfeight balance, but subtract paid out forfeitures
      s.forfeiture += (asset(balance_forfeited,symbol) - asset(bonus_vested,symbol)); 
   });

   //erase hodlaccts table
   from_acnts.erase(from);

   //must have opened a balance of DAPP to receive the transfer
   token_accounts token_acnts(TOKEN,owner.value);
   const auto& token = token_acnts.get( sym_code_raw, "no destination balance found. please open an account with dappservices" );

   //transfer tokens
   action(permission_level{_self, "active"_n}, 
      TOKEN, "transfer"_n,
      std::make_tuple(_self, owner, payout, std::string("Withdrawal from AirHODL")))
   .send();
}

void airhodl::stake( name owner, name provider, name service, asset quantity) {
   require_auth(owner);

   //add stake asserts if they have enough available tokens
   add_stake(owner,quantity);

   //perform third party staking
   action(permission_level{_self, "active"_n}, 
      TOKEN, "staketo"_n,
      std::make_tuple(_self, owner, provider, service, quantity))
   .send();
}

void airhodl::unstake( name owner, name provider, name service, asset quantity) {
   require_auth(owner);

   //perform third party unstaking
   //we wont bother asserting here if they are attempting to unstake too much
   //because the inline action will assert that
   //we sub the stake once the refund request is successful
   action(permission_level{_self, "active"_n}, 
      TOKEN, "unstaketo"_n,
      std::make_tuple(_self, owner, provider, service, quantity))
   .send();
}

void airhodl::refund( name owner, name provider, name service, symbol_code symcode) {
   //require_auth(owner);
   action(permission_level{_self, "active"_n}, 
      TOKEN, "refundto"_n,
      std::make_tuple(_self, owner, provider, service, symcode))
   .send();
}

void airhodl::on_receipt(name from, name to, asset quantity) {
   if(from == _self) {
      sub_stake(to,quantity);
   }   
}

void airhodl::add_balance( name owner, asset value, name ram_payer )
{
   accounts to_acnts( _self, owner.value );
   auto to = to_acnts.find( value.symbol.code().raw() );
   if( to == to_acnts.end() ) {
      to_acnts.emplace( ram_payer, [&]( auto& a ){
        a.balance = value;
        a.allocation = value;
        a.staked.symbol = value.symbol;
        a.claimed = false;
      });
   } else {
      to_acnts.modify( to, eosio::same_payer, [&]( auto& a ) {
        a.balance += value;
      });
   }
}

void airhodl::add_stake( name owner, asset value ) {
   accounts from_acnts( _self, owner.value );

   const auto& from = from_acnts.get( value.symbol.code().raw(), "no balance object found" );   

   //Find token stats
   auto sym_code_raw = value.symbol.code().raw();

   stats statstable( _self, sym_code_raw );
   auto existing = statstable.find( sym_code_raw );
   eosio::check( existing != statstable.end(), "symbol does not exist for withdrawal" );
   const auto& st = *existing;

   asset bonus = asset(0,st.supply.symbol);
   asset diff = asset(0,st.supply.symbol);

   //Check if vesting has started
   if(st.vesting_start > time_point()) {
      //calculate vesting ratio
      auto time_elapsed = current_time_point() - st.vesting_start;
      auto vesting_duration = st.vesting_end - st.vesting_start;
      double vesting_ratio = double(time_elapsed.count()) / double(vesting_duration.count());


      //calculate the bonus amount
      double   bonus_share = double(st.forfeiture.amount) * (double(from.allocation.amount) / double(st.supply.amount));
      uint64_t bonus_vested = static_cast<uint64_t>(vesting_ratio * bonus_share);
      bonus.amount = bonus_vested;
   }

   //Find difference of new bonus vs old bonus
   diff.amount = (from.allocation.amount + bonus.amount) - (from.balance.amount + from.staked.amount);
   eosio::check( from.balance.amount + diff.amount >= value.amount, "overdrawn balance" );

   if(from.claimed) {
      from_acnts.modify( from, owner, [&]( auto& a ) {
         a.balance += (diff - value);
         a.staked += value;
      });
   } else {
      //lets perform a grab if they haven't yet
      asset balance = from.balance + (diff - value);
      asset staked  = from.staked + value;
      asset allocation = from.allocation;
      from_acnts.erase(from);

      from_acnts.emplace( owner, [&]( auto& a ){
         a.balance = balance;
         a.allocation = allocation;
         a.staked  = staked;
         a.claimed = true;
      });
   }   
}

void airhodl::sub_stake( name owner, asset value )
{
   accounts from_acnts( _self, owner.value );

   const auto& from = from_acnts.get( value.symbol.code().raw(), "no balance object found" );
   eosio::check( from.staked.amount >= value.amount, "overdrawn stake" );

   from_acnts.modify( from, owner, [&]( auto& a ) {
      a.balance += value;
      a.staked -= value;
   });
}

// extern "C" void apply( uint64_t receiver, uint64_t code, uint64_t action ) {
//   if( code == TOKEN.value && action == name("refreceipt").value ) {
//     execute_action<airhodl>( name(receiver), name(code), &airhodl::refunded );
//   } else if( code == receiver ) {
//     switch( action ) {
//       EOSIO_DISPATCH_HELPER( airhodl, (create)(issue)(activate)
//                                           (grab)(withdraw)(stake)(unstake)(refund));
//     }                                       
//   }
// }

} /// namespace airhodl

