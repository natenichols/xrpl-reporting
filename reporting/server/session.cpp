#include <reporting/server/session.h>

void
fail(boost::beast::error_code ec, char const* what)
{
    std::cerr << what << ": " << ec.message() << "\n";
}

boost::json::object
buildResponse(
    boost::json::object const& request,
    CassandraFlatMapBackend const& backend,
    std::shared_ptr<PgPool>& pgPool,
    SubscriptionManager& subManager,
    std::shared_ptr<session> session)
{
    std::string command = request.at("command").as_string().c_str();
    boost::json::object response;
    switch (commandMap[command])
    {
        case tx:
            return doTx(request, backend, pgPool);
            break;
        case account_tx:
            return doAccountTx(request, backend, pgPool);
            break;
        case book_offers:
            return doBookOffers(request, backend, pgPool);
            break;
        case ledger:
            break;
        case ledger_data:
            return doLedgerData(request, backend);
            break;
        case account_info:
            return doAccountInfo(request, backend, pgPool);
            break;
        case subscribe:
            return doSubscribe(request, session, subManager);
            break;
        case unsubscribe:
            return doUnsubscribe(request, session, subManager);
            break;
        default:
            BOOST_LOG_TRIVIAL(error) << "Unknown command: " << command;
    }
    return response;
}