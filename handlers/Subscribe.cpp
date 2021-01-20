#include <boost/json.hpp>
#include <reporting/server/session.h>

boost::json::object
doSubscribe(
    boost::json::object const& request,
    std::shared_ptr<session>& session,
    SubscriptionManager& manager)
{
    boost::json::object response;

    manager.subLedger(session);

    response["status"] = "success";
    return response;
}

boost::json::object
doUnsubscribe(
    boost::json::object const& request,
    std::shared_ptr<session>& session,
    SubscriptionManager& manager)
{
    boost::json::object response;

    manager.unsubLedger(session);

    response["status"] = "success";
    return response;
}