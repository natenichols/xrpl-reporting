#include<reporting/server/SubscriptionManager.h>
    
void
SubscriptionManager::subLedger(std::shared_ptr<session>& session)
{
    subscribers_[Ledgers].emplace(std::move(session));
}

void
SubscriptionManager::unsubLedger(std::shared_ptr<session>& session)
{
    subscribers_[Ledgers].erase(session);
}


void
SubscriptionManager::pubLedger(ripple::LedgerInfo const& lgrInfo)
{
    for (auto const& session: subscribers_[Ledgers])
    {
        boost::json::object subMsg;

        subMsg["type"] = "ledgerClosed";
        subMsg["ledger_index"] = lgrInfo.seq;
        subMsg["ledger_hash"] = to_string(lgrInfo.hash);
        subMsg["ledger_time"] = lgrInfo.closeTime.time_since_epoch().count();

        // jvObj[jss::fee_ref] = lpAccepted->fees().units.jsonClipped();
        // jvObj[jss::fee_base] = lpAccepted->fees().base.jsonClipped();
        // jvObj[jss::reserve_base] =
        //     lpAccepted->fees().accountReserve(0).jsonClipped();
        // jvObj[jss::reserve_inc] =
        //     lpAccepted->fees().increment.jsonClipped();

        // jvObj[jss::txn_count] = Json::UInt(alpAccepted->getTxnCount());
        // jvObj[jss::validated_ledgers]

        // subMsg["fee_base"] = 10;
        // subMsg["fee_ref"] = 10;
        // subMsg["reserve_base"] = 20000000;
        // subMsg["reserve_inc"] = 5000000;
        // subMsg["txn_count"] = 54;
        // subMsg["validated_ledgers"] = "60273114-61009970";

        session->send(boost::json::serialize(subMsg));
    }
}