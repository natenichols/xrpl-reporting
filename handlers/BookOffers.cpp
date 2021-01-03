#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/STLedgerEntry.h>
#include <boost/json.hpp>
#include <ripple/protocol/jss.h>
#include <ripple/protocol/ErrorCodes.h>
#include <reporting/ReportingBackend.h>
#include <reporting/DBHelpers.h>
#include <reporting/Pg.h>
#include <ripple/app/ledger/Ledger.h>

std::optional<std::uint32_t>
ledgerSequenceFromRequest(
    boost::json::object const& request,
    std::shared_ptr<PgPool> const& pool)
{
    std::stringstream sql;
    sql << "SELECT ledger_seq FROM ledgers WHERE ";

    if (auto p = request.at("ledger_index").is_uint64())
    {
        sql << "ledger_seq = " << std::to_string(p);
    }
    else if (auto p = request.at("ledger_index").is_string())
    {
        //Do max Seq stuff
    }
    else if (auto p = request.at("ledger_hash").is_string())
    {
        sql << "ledger_hash = \\\\x" << p;
    }

    sql << ";";

    auto index = PgQuery(pool)(sql.str().c_str());
    if (!index || index.isNull())
        return {};
    
    return std::optional<std::uint32_t>{index.asInt()};
}

std::vector<void const*>
loadBookOfferIndexes(ripple::Book book, std::uint32_t seq, std::uint32_t limit, std::shared_ptr<PgPool> const& pool)
{
    std::vector<void const*> hashes = {};

    ripple::uint256 bookBase = getBookBase(book);
    ripple::uint256 bookEnd = getQualityNext(bookBase);

    std::stringstream sql;
    sql << "SELECT offer_indexes FROM books "
        << "WHERE book_directory >= " << ripple::strHex(bookBase) << " "
        << "AND book_directory < " << ripple::strHex(bookEnd) << " "
        << "AND ledger_index <= " << std::to_string(seq) << " "
        << "LIMIT " << std::to_string(limit) << ";";

    auto indexes = PgQuery(pool)(sql.str().c_str());
    if (!indexes || indexes.isNull())
        return {};

    for (auto i = 0; i < indexes.ntuples(); ++i)
    {
        hashes.push_back(indexes.c_str(i));
    }

    return hashes;
}

boost::json::object
doBookOffers(
    boost::json::object const& request,
    CassandraFlatMapBackend& backend,
    std::shared_ptr<PgPool> const& pool)
{
    boost::json::object response;
    auto sequence = ledgerSequenceFromRequest(request, pool);

    if (!sequence)
        return response;

    if (!request.contains("taker_pays"))
    {
        response["error"] = "Missing field taker_pays";
        return response;
    }

    if (!request.contains("taker_gets"))
    {
        response["error"] = "Missing field taker_gets";
        return response;
    }

    boost::json::object taker_pays;
    if (auto taker = request.at("taker_pays").is_object())
    {
        taker_pays = taker;
    }
    else
    {
        response["error"] = "Invalid field taker_pays";
        return response;
    }

    boost::json::object taker_gets;
    if (auto getter = request.at("taker_gets").is_object())
    {
        taker_gets = getter;
    }
    else
    {
        response["error"] = "Invalid field taker_gets";
        return response;
    }

    if (!taker_pays.contains("currency"))
    {
        response["error"] = "Missing field taker_pays.currency";
        return response;
    }

    if (!taker_pays["currency"].is_string())
    {
        response["error"] = "taker_pays.currency should be string";
        return response;
    }

    if (!taker_gets.contains("currency"))
    {
        response["error"] = "Missing field taker_gets.currency";
        return response;
    }

    if (!taker_gets.at("currency").is_string())
    {
        response["error"] = "taker_gets.currency should be string";
        return response;
    }

    ripple::Currency pay_currency;

    if (!ripple::to_currency(pay_currency, taker_pays.at("currency").as_string().c_str()))
    {
        response["error"] = "Invalid field 'taker_pays.currency', bad currency.";
        return response;
    }

    ripple::Currency get_currency;

    if (!ripple::to_currency(get_currency, taker_gets["currency"].as_string().c_str()))
    {
        response["error"] = "Invalid field 'taker_gets.currency', bad currency.";
        return response;
    }

    ripple::AccountID pay_issuer;

    if (taker_pays.contains("issuer"))
    {
        if (!taker_pays.at("issuer").is_string())
        {
            response["error"] = "taker_pays.issuer should be string";
            return response;
        }

        if (!ripple::to_issuer(pay_issuer, taker_pays.at("issuer").as_string().c_str()))
        {
            response["error"] = "Invalid field 'taker_pays.issuer', bad issuer.";
            return response;
        }

        if (pay_issuer == ripple::noAccount())
        {
            response["error"] =
                "Invalid field 'taker_pays.issuer', bad issuer account one.";
            return response;
        }
    }
    else
    {
        pay_issuer = ripple::xrpAccount();
    }

    if (isXRP(pay_currency) && !isXRP(pay_issuer))
    {
        response["error"] =
            "Unneeded field 'taker_pays.issuer' for XRP currency specification.";
        return response;
    }

    if (!isXRP(pay_currency) && isXRP(pay_issuer))
    {
        response["error"] =
            "Invalid field 'taker_pays.issuer', expected non-XRP issuer.";
        return response;
    }

    ripple::AccountID get_issuer;

    if (taker_gets.contains("issuer"))
    {
        if (!taker_gets["issuer"].is_string())
        {
            response["error"] = "taker_gets.issuer should be string";
            return response;
        }

        if (!ripple::to_issuer(get_issuer, taker_gets.at("issuer").as_string().c_str()))
        {
            response["error"] =
                "Invalid field 'taker_gets.issuer', bad issuer.";
            return response;
        }

        if (get_issuer == ripple::noAccount())
        {
            response["error"] =
                "Invalid field 'taker_gets.issuer', bad issuer account one.";
            return response;
        }
    }
    else
    {
        get_issuer = ripple::xrpAccount();
    }

    if (ripple::isXRP(get_currency) && !ripple::isXRP(get_issuer))
    {
        response["error"] =
            "Unneeded field 'taker_gets.issuer' for XRP currency specification.";
        return response;
    }

    if (!ripple::isXRP(get_currency) && ripple::isXRP(get_issuer))
    {
        response["error"] = 
            "Invalid field 'taker_gets.issuer', expected non-XRP issuer.";
        return response;
    }

    boost::optional<ripple::AccountID> takerID;
    if (request.contains("taker"))
    {
        if (!request.at("taker").is_string())
        {
            response["error"] = "taker should be string";
            return response;
        }

        takerID = ripple::parseBase58<ripple::AccountID>(request.at("taker").as_string().c_str());
        if (!takerID)
        {
            response["error"] = "Invalid taker";
            return response;
        }
    }

    if (pay_currency == get_currency && pay_issuer == get_issuer)
    {
        response["error"] = "Bad market";
        return response;
    }

    std::uint32_t limit = 2048;
    if(auto p = request.at("limit").is_uint64())
        limit = p;

    ripple::Book book = {{pay_currency, pay_issuer}, {get_currency, get_issuer}};

    auto hashes = loadBookOfferIndexes(book, *sequence, limit, pool);

    response["offers"] = boost::json::value(boost::json::array_kind);
    boost::json::array& jsonOffers = response.at("offers").as_array();

    std::vector<std::vector<unsigned char>> offers;
    for (auto const& hash : hashes)
    {
        auto bytes = backend.fetch(hash, *sequence);
        if(bytes)
            offers.push_back(*bytes);
    }

    std::transform(hashes.begin(), hashes.end(),
                   offers.begin(),
                   std::back_inserter(jsonOffers),
                   [](auto hash, auto offerBytes){
                       ripple::SerialIter it(offerBytes.data(), offerBytes.size());
                       ripple::SLE offer{it, ripple::uint256::fromVoid(hash)};
                       std::string json =
                            offer.getJson(ripple::JsonOptions::none).toStyledString();
                       return boost::json::parse(json);
                   });

    return response;
}