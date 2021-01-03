//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2020 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <reporting/DBHelpers.h>
#include <reporting/ReportingETL.h>
#include <ripple/basics/StringUtilities.h>

#include <ripple/beast/core/CurrentThreadName.h>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <cstdlib>
#include <iostream>
#include <string>
#include <variant>

namespace detail {
/// Convenience function for printing out basic ledger info
std::string
toString(ripple::LedgerInfo const& info)
{
    std::stringstream ss;
    ss << "LedgerInfo { Sequence : " << info.seq
       << " Hash : " << strHex(info.hash) << " TxHash : " << strHex(info.txHash)
       << " AccountHash : " << strHex(info.accountHash)
       << " ParentHash : " << strHex(info.parentHash) << " }";
    return ss.str();
}
ripple::LedgerInfo
deserializeHeader(ripple::Slice data)
{
    ripple::SerialIter sit(data.data(), data.size());

    ripple::LedgerInfo info;

    info.seq = sit.get32();
    info.drops = sit.get64();
    info.parentHash = sit.get256();
    info.txHash = sit.get256();
    info.accountHash = sit.get256();
    info.parentCloseTime =
        ripple::NetClock::time_point{ripple::NetClock::duration{sit.get32()}};
    info.closeTime =
        ripple::NetClock::time_point{ripple::NetClock::duration{sit.get32()}};
    info.closeTimeResolution = ripple::NetClock::duration{sit.get8()};
    info.closeFlags = sit.get8();

    info.hash = sit.get256();

    return info;
}
}  // namespace detail

std::vector<AccountTransactionsData>
ReportingETL::insertTransactions(
    ripple::LedgerInfo const& ledger,
    org::xrpl::rpc::v1::GetLedgerResponse& data)
{
    std::vector<AccountTransactionsData> accountTxData;
    for (auto& txn :
         *(data.mutable_transactions_list()->mutable_transactions()))
    {
        std::string* raw = txn.mutable_transaction_blob();

        ripple::SerialIter it{raw->data(), raw->size()};
        ripple::STTx sttx{it};

        auto txSerializer =
            std::make_shared<ripple::Serializer>(sttx.getSerializer());

        ripple::TxMeta txMeta{
            sttx.getTransactionID(), ledger.seq, txn.metadata_blob()};

        auto metaSerializer = std::make_shared<ripple::Serializer>(
            txMeta.getAsObject().getSerializer());

        BOOST_LOG_TRIVIAL(trace)
            << __func__ << " : "
            << "Inserting transaction = " << sttx.getTransactionID();
        ripple::uint256 nodestoreHash = sttx.getTransactionID();

        auto journal = ripple::debugLog();
        accountTxData.emplace_back(txMeta, std::move(nodestoreHash), journal);
        std::string keyStr{(const char*)sttx.getTransactionID().data(), 32};
        flatMapBackend_.storeTransaction(
            std::move(keyStr),
            ledger.seq,
            std::move(*raw),
            std::move(*txn.mutable_metadata_blob()));
    }
    return accountTxData;
}

std::optional<BookDirectoryData>
ReportingETL::handleOffer(std::string const& keyString, std::uint32_t seq, std::string const& offer)
{
    auto key = ripple::uint256::fromVoid(keyString.data());
    ripple::SerialIter it{offer.data(), offer.size()};
    ripple::SLE sle{it, key};

    return BookDirectoryData(sle, seq);
}

std::vector<BookDirectoryData>
ReportingETL::insertObjects(
    ripple::LedgerInfo const& ledger,
    org::xrpl::rpc::v1::GetLedgerResponse& rawData)
{
    std::vector<BookDirectoryData> dirData = {};
    for (auto& obj : *(rawData.mutable_ledger_objects()->mutable_objects()))
    {
        if(*obj.mutable_data() == "")
            continue;

        std::string str = *obj.mutable_data();
        short offer_bytes = (str[1] << 8) | str[2];

        if(offer_bytes == 0x006f)
        {
            auto data = handleOffer(*obj.mutable_key(), ledger.seq, *obj.mutable_data());
            if (data)
                dirData.push_back(*data);
        }

        flatMapBackend_.store(
            std::move(*obj.mutable_key()),
            ledger.seq,
            std::move(*obj.mutable_data()));
    }

    return dirData;
}

std::vector<BookDirectoryData>
ReportingETL::insertObjects(
    ripple::LedgerInfo const& ledger,
    org::xrpl::rpc::v1::GetLedgerDataResponse& rawData)
{
    std::vector<BookDirectoryData> dirData = {};
    for (auto& obj : *(rawData.mutable_ledger_objects()->mutable_objects()))
    {
        if(*obj.mutable_data() == "")
            continue;

        std::string str = *obj.mutable_data();
        short offer_bytes = (str[1] << 8) | str[2];

        if(offer_bytes == 0x006f)
        {
            auto data = handleOffer(*obj.mutable_key(), ledger.seq, *obj.mutable_data());
            if (data)
                dirData.push_back(*data);
        }

        flatMapBackend_.store(
            std::move(*obj.mutable_key()),
            ledger.seq,
            std::move(*obj.mutable_data()));
    }

    return dirData;
}

std::optional<ripple::LedgerInfo>
ReportingETL::loadInitialLedger(uint32_t startingSequence)
{
    // check that database is actually empty
    auto ledger = getLedger(startingSequence, pgPool_);
    if (ledger)
    {
        BOOST_LOG_TRIVIAL(fatal) << __func__ << " : "
                                 << "Database is not empty";
        assert(false);
        return {};
    }

    // fetch the ledger from the network. This function will not return until
    // either the fetch is successful, or the server is being shutdown. This
    // only fetches the ledger header and the transactions+metadata
    std::optional<org::xrpl::rpc::v1::GetLedgerResponse> ledgerData{
        fetchLedgerData(startingSequence)};
    if (!ledgerData)
        return {};

    ripple::LedgerInfo lgrInfo = detail::deserializeHeader(
        ripple::makeSlice(ledgerData->ledger_header()));

    BOOST_LOG_TRIVIAL(debug)
        << __func__ << " : "
        << "Deserialized ledger header. " << detail::toString(lgrInfo);

    std::vector<AccountTransactionsData> accountTxData =
        insertTransactions(lgrInfo, *ledgerData);

    auto start = std::chrono::system_clock::now();

    ThreadSafeQueue<std::optional<org::xrpl::rpc::v1::GetLedgerDataResponse>> writeQueue;
    std::vector<BookDirectoryData> bookDirData = {};

    std::thread AsyncWriter([this, &bookDirData, &writeQueue, &lgrInfo](){
      std::optional<org::xrpl::rpc::v1::GetLedgerDataResponse> data;
      size_t num = 0;

      while(!stopping_ && (data = writeQueue.pop()))
      {
        if(!data)
          break;

        auto booksToWrite = insertObjects(lgrInfo, *data);

        // After written to ReportingBackend, save books to write to postgres
        bookDirData.insert(
          bookDirData.end(),
          std::make_move_iterator(booksToWrite.begin()),
          std::make_move_iterator(booksToWrite.end()));
      }
    });

    // download the full account state map. This function downloads full ledger
    // data and pushes the downloaded data into the writeQueue. asyncWriter
    // consumes from the queue and inserts the data into the Ledger object.
    // Once the below call returns, all data has been pushed into the queue
    loadBalancer_.loadInitialLedger(startingSequence, writeQueue);

    writeQueue.push({});

    if (!stopping_)
    {
        flatMapBackend_.sync();
        writeToPostgres(lgrInfo, accountTxData, pgPool_);
        writeToPostgres(bookDirData, pgPool_);
    }
    auto end = std::chrono::system_clock::now();

    AsyncWriter.join();

    BOOST_LOG_TRIVIAL(debug) << "Time to download and store ledger = "
                             << ((end - start).count()) / 1000000000.0;
    return lgrInfo;
}

void
ReportingETL::publishLedger(ripple::LedgerInfo const& lgrInfo)
{
    // app_.getOPs().pubLedger(ledger);

    setLastPublish();
}

bool
ReportingETL::publishLedger(uint32_t ledgerSequence, uint32_t maxAttempts)
{
    BOOST_LOG_TRIVIAL(info)
        << __func__ << " : "
        << "Attempting to publish ledger = " << ledgerSequence;
    size_t numAttempts = 0;
    while (!stopping_)
    {
        auto ledger = getLedger(ledgerSequence, pgPool_);

        if (!ledger)
        {
            BOOST_LOG_TRIVIAL(warning)
                << __func__ << " : "
                << "Trying to publish. Could not find ledger with sequence = "
                << ledgerSequence;
            // We try maxAttempts times to publish the ledger, waiting one
            // second in between each attempt.
            // If the ledger is not present in the database after maxAttempts,
            // we attempt to take over as the writer. If the takeover fails,
            // doContinuousETL will return, and this node will go back to
            // publishing.
            // If the node is in strict read only mode, we simply
            // skip publishing this ledger and return false indicating the
            // publish failed
            if (numAttempts >= maxAttempts)
            {
                BOOST_LOG_TRIVIAL(error) << __func__ << " : "
                                         << "Failed to publish ledger after "
                                         << numAttempts << " attempts.";
                if (!readOnly_)
                {
                    BOOST_LOG_TRIVIAL(info)
                        << __func__ << " : "
                        << "Attempting to become ETL writer";
                    return false;
                }
                else
                {
                    BOOST_LOG_TRIVIAL(debug)
                        << __func__ << " : "
                        << "In strict read-only mode. "
                        << "Skipping publishing this ledger. "
                        << "Beginning fast forward.";
                    return false;
                }
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                ++numAttempts;
            }
            continue;
        }

        /*
        publishStrand_.post([this, ledger]() {
            app_.getOPs().pubLedger(ledger);
            setLastPublish();
            BOOST_LOG_TRIVIAL(info)
                << __func__ << " : "
                << "Published ledger. " << detail::toString(ledger->info());
        });
        */
        return true;
    }
    return false;
}

std::optional<org::xrpl::rpc::v1::GetLedgerResponse>
ReportingETL::fetchLedgerData(uint32_t idx)
{
    BOOST_LOG_TRIVIAL(debug)
        << __func__ << " : "
        << "Attempting to fetch ledger with sequence = " << idx;

    std::optional<org::xrpl::rpc::v1::GetLedgerResponse> response =
        loadBalancer_.fetchLedger(idx, false);
    BOOST_LOG_TRIVIAL(trace) << __func__ << " : "
                             << "GetLedger reply = " << response->DebugString();
    return response;
}

std::optional<org::xrpl::rpc::v1::GetLedgerResponse>
ReportingETL::fetchLedgerDataAndDiff(uint32_t idx)
{
    BOOST_LOG_TRIVIAL(debug)
        << __func__ << " : "
        << "Attempting to fetch ledger with sequence = " << idx;

    std::optional<org::xrpl::rpc::v1::GetLedgerResponse> response =
        loadBalancer_.fetchLedger(idx, true);
    BOOST_LOG_TRIVIAL(trace) << __func__ << " : "
                             << "GetLedger reply = " << response->DebugString();
    return response;
}

std::tuple<ripple::LedgerInfo, std::vector<AccountTransactionsData>, std::vector<BookDirectoryData>>
ReportingETL::buildNextLedger(org::xrpl::rpc::v1::GetLedgerResponse& rawData)
{
    BOOST_LOG_TRIVIAL(info) << __func__ << " : "
                            << "Beginning ledger update";

    ripple::LedgerInfo lgrInfo =
        detail::deserializeHeader(ripple::makeSlice(rawData.ledger_header()));

    BOOST_LOG_TRIVIAL(debug)
        << __func__ << " : "
        << "Deserialized ledger header. " << detail::toString(lgrInfo);

    std::vector<AccountTransactionsData> accountTxData{
        insertTransactions(lgrInfo, rawData)};

    BOOST_LOG_TRIVIAL(debug)
        << __func__ << " : "
        << "Inserted all transactions. Number of transactions  = "
        << rawData.transactions_list().transactions_size();

    std::vector<BookDirectoryData> bookDirData{insertObjects(lgrInfo, rawData)};

    flatMapBackend_.sync();
    BOOST_LOG_TRIVIAL(debug)
        << __func__ << " : "
        << "Inserted/modified/deleted all objects. Number of objects = "
        << rawData.ledger_objects().objects_size();

    BOOST_LOG_TRIVIAL(debug)
        << __func__ << " : "
        << "Finished ledger update. " << detail::toString(lgrInfo);
    return {lgrInfo, std::move(accountTxData), std::move(bookDirData)};
}

// Database must be populated when this starts
std::optional<uint32_t>
ReportingETL::runETLPipeline(uint32_t startSequence)
{
    /*
     * Behold, mortals! This function spawns three separate threads, which talk
     * to each other via 2 different thread safe queues and 1 atomic variable.
     * All threads and queues are function local. This function returns when all
     * of the threads exit. There are two termination conditions: the first is
     * if the load thread encounters a write conflict. In this case, the load
     * thread sets writeConflict, an atomic bool, to true, which signals the
     * other threads to stop. The second termination condition is when the
     * entire server is shutting down, which is detected in one of three ways:
     * 1. isStopping() returns true if the server is shutting down
     * 2. networkValidatedLedgers_.waitUntilValidatedByNetwork returns
     * false, signaling the wait was aborted.
     * 3. fetchLedgerDataAndDiff returns an empty optional, signaling the fetch
     * was aborted.
     * In all cases, the extract thread detects this condition,
     * and pushes an empty optional onto the transform queue. The transform
     * thread, upon popping an empty optional, pushes an empty optional onto the
     * load queue, and then returns. The load thread, upon popping an empty
     * optional, returns.
     */

    BOOST_LOG_TRIVIAL(debug) << __func__ << " : "
                             << "Starting etl pipeline";
    writing_ = true;

    auto parent = getLedger(startSequence - 1, pgPool_);
    if (!parent)
    {
        assert(false);
        throw std::runtime_error("runETLPipeline: parent ledger is null");
    }

    std::atomic_bool writeConflict = false;
    std::optional<uint32_t> lastPublishedSequence;
    constexpr uint32_t maxQueueSize = 1000;
    auto begin = std::chrono::system_clock::now();

    ThreadSafeQueue<std::optional<org::xrpl::rpc::v1::GetLedgerResponse>>
        transformQueue{maxQueueSize};

    std::thread extracter{[this,
                           &startSequence,
                           &writeConflict,
                           &transformQueue]() {
        beast::setCurrentThreadName("rippled: ReportingETL extract");
        uint32_t currentSequence = startSequence;

        // there are two stopping conditions here.
        // First, if there is a write conflict in the load thread, the ETL
        // mechanism should stop.
        // The other stopping condition is if the entire server is shutting
        // down. This can be detected in a variety of ways. See the comment
        // at the top of the function
        while (networkValidatedLedgers_.waitUntilValidatedByNetwork(
                   currentSequence) &&
               !writeConflict && !isStopping())
        {
            auto start = std::chrono::system_clock::now();
            std::optional<org::xrpl::rpc::v1::GetLedgerResponse> fetchResponse{
                fetchLedgerDataAndDiff(currentSequence)};
            auto end = std::chrono::system_clock::now();

            auto time = ((end - start).count()) / 1000000000.0;
            auto tps =
                fetchResponse->transactions_list().transactions_size() / time;

            BOOST_LOG_TRIVIAL(debug) << "Extract phase time = " << time
                                     << " . Extract phase tps = " << tps;
            // if the fetch is unsuccessful, stop. fetchLedger only returns
            // false if the server is shutting down, or if the ledger was
            // found in the database (which means another process already
            // wrote the ledger that this process was trying to extract;
            // this is a form of a write conflict). Otherwise,
            // fetchLedgerDataAndDiff will keep trying to fetch the
            // specified ledger until successful
            if (!fetchResponse)
            {
                break;
            }

            transformQueue.push(std::move(fetchResponse));
            ++currentSequence;
        }
        // empty optional tells the transformer to shut down
        transformQueue.push({});
    }};

    std::thread transformer{[this,
                             &writeConflict,
                             &transformQueue,
                             &lastPublishedSequence]() {
        beast::setCurrentThreadName("rippled: ReportingETL transform");

        while (!writeConflict)
        {
            std::optional<org::xrpl::rpc::v1::GetLedgerResponse> fetchResponse{
                transformQueue.pop()};
            // if fetchResponse is an empty optional, the extracter thread
            // has stopped and the transformer should stop as well
            if (!fetchResponse)
            {
                break;
            }
            if (isStopping())
                continue;

            auto start = std::chrono::system_clock::now();
            auto [lgrInfo, accountTxData, bookData] = buildNextLedger(*fetchResponse);

            auto end = std::chrono::system_clock::now();
            if (!writeToPostgres(lgrInfo, accountTxData, pgPool_) || !writeToPostgres(bookData, pgPool_))
                writeConflict = true;

            auto duration = ((end - start).count()) / 1000000000.0;
            auto numTxns = accountTxData.size();
            BOOST_LOG_TRIVIAL(info)
                << "Load phase of etl : "
                << "Successfully published ledger! Ledger info: "
                << detail::toString(lgrInfo) << ". txn count = " << numTxns
                << ". load time = " << duration << ". load tps "
                << numTxns / duration;
            if (!writeConflict)
            {
                publishLedger(lgrInfo);
                lastPublishedSequence = lgrInfo.seq;
            }
        }
    }};

    // wait for all of the threads to stop
    extracter.join();
    transformer.join();
    auto end = std::chrono::system_clock::now();
    BOOST_LOG_TRIVIAL(debug)
        << "Extracted and wrote " << *lastPublishedSequence - startSequence
        << " in " << ((end - begin).count()) / 1000000000.0;
    writing_ = false;

    BOOST_LOG_TRIVIAL(debug) << __func__ << " : "
                             << "Stopping etl pipeline";

    return lastPublishedSequence;
}

// main loop. The software begins monitoring the ledgers that are validated
// by the nework. The member networkValidatedLedgers_ keeps track of the
// sequences of ledgers validated by the network. Whenever a ledger is validated
// by the network, the software looks for that ledger in the database. Once the
// ledger is found in the database, the software publishes that ledger to the
// ledgers stream. If a network validated ledger is not found in the database
// after a certain amount of time, then the software attempts to take over
// responsibility of the ETL process, where it writes new ledgers to the
// database. The software will relinquish control of the ETL process if it
// detects that another process has taken over ETL.
void
ReportingETL::monitor()
{
    auto ledger = getLedger(std::monostate(), pgPool_);
    if (!ledger)
    {
        BOOST_LOG_TRIVIAL(info) << __func__ << " : "
                                << "Database is empty. Will download a ledger "
                                   "from the network.";
        if (startSequence_)
        {
            BOOST_LOG_TRIVIAL(info)
                << __func__ << " : "
                << "ledger sequence specified in config. "
                << "Will begin ETL process starting with ledger "
                << *startSequence_;
            ledger = loadInitialLedger(*startSequence_);
        }
        else
        {
            BOOST_LOG_TRIVIAL(info)
                << __func__ << " : "
                << "Waiting for next ledger to be validated by network...";
            std::optional<uint32_t> mostRecentValidated =
                networkValidatedLedgers_.getMostRecent();
            if (mostRecentValidated)
            {
                BOOST_LOG_TRIVIAL(info) << __func__ << " : "
                                        << "Ledger " << *mostRecentValidated
                                        << " has been validated. "
                                        << "Downloading...";
                ledger = loadInitialLedger(*mostRecentValidated);
            }
            else
            {
                BOOST_LOG_TRIVIAL(info) << __func__ << " : "
                                        << "The wait for the next validated "
                                        << "ledger has been aborted. "
                                        << "Exiting monitor loop";
                return;
            }
        }
    }
    else
    {
        if (startSequence_)
        {
            throw std::runtime_error(
                "start sequence specified but db is already populated");
        }
        BOOST_LOG_TRIVIAL(info)
            << __func__ << " : "
            << "Database already populated. Picking up from the tip of history";
    }
    if (!ledger)
    {
        BOOST_LOG_TRIVIAL(error)
            << __func__ << " : "
            << "Failed to load initial ledger. Exiting monitor loop";
        return;
    }
    else
    {
        // publishLedger(ledger);
    }
    uint32_t nextSequence = ledger->seq + 1;

    BOOST_LOG_TRIVIAL(debug)
        << __func__ << " : "
        << "Database is populated. "
        << "Starting monitor loop. sequence = " << nextSequence;
    while (!stopping_ &&
           networkValidatedLedgers_.waitUntilValidatedByNetwork(nextSequence))
    {
        BOOST_LOG_TRIVIAL(info) << __func__ << " : "
                                << "Ledger with sequence = " << nextSequence
                                << " has been validated by the network. "
                                << "Attempting to find in database and publish";
        // Attempt to take over responsibility of ETL writer after 10 failed
        // attempts to publish the ledger. publishLedger() fails if the
        // ledger that has been validated by the network is not found in the
        // database after the specified number of attempts. publishLedger()
        // waits one second between each attempt to read the ledger from the
        // database
        //
        // In strict read-only mode, when the software fails to find a
        // ledger in the database that has been validated by the network,
        // the software will only try to publish subsequent ledgers once,
        // until one of those ledgers is found in the database. Once the
        // software successfully publishes a ledger, the software will fall
        // back to the normal behavior of trying several times to publish
        // the ledger that has been validated by the network. In this
        // manner, a reporting processing running in read-only mode does not
        // need to restart if the database is wiped.
        constexpr size_t timeoutSeconds = 10;
        bool success = publishLedger(nextSequence, timeoutSeconds);
        if (!success)
        {
            BOOST_LOG_TRIVIAL(warning)
                << __func__ << " : "
                << "Failed to publish ledger with sequence = " << nextSequence
                << " . Beginning ETL";
            // doContinousETLPipelined returns the most recent sequence
            // published empty optional if no sequence was published
            std::optional<uint32_t> lastPublished =
                runETLPipeline(nextSequence);
            BOOST_LOG_TRIVIAL(info)
                << __func__ << " : "
                << "Aborting ETL. Falling back to publishing";
            // if no ledger was published, don't increment nextSequence
            if (lastPublished)
                nextSequence = *lastPublished + 1;
        }
        else
        {
            ++nextSequence;
        }
    }
}

void
ReportingETL::monitorReadOnly()
{
    BOOST_LOG_TRIVIAL(debug) << "Starting reporting in strict read only mode";
    std::optional<uint32_t> mostRecent =
        networkValidatedLedgers_.getMostRecent();
    if (!mostRecent)
        return;
    uint32_t sequence = *mostRecent;
    bool success = true;
    while (!stopping_ &&
           networkValidatedLedgers_.waitUntilValidatedByNetwork(sequence))
    {
        success = publishLedger(sequence, success ? 30 : 1);
        ++sequence;
    }
}

void
ReportingETL::doWork()
{
    worker_ = std::thread([this]() {
        beast::setCurrentThreadName("rippled: ReportingETL worker");
        if (readOnly_)
            monitorReadOnly();
        else
            monitor();
    });
}

ReportingETL::ReportingETL(
    boost::json::object const& config,
    boost::asio::io_context& ioc)
    : publishStrand_(ioc)
    , ioContext_(ioc)
    , flatMapBackend_(
          config.at("database").as_object().at("cassandra").as_object())
    , pgPool_(make_PgPool(
          config.at("database").as_object().at("postgres").as_object()))
    , loadBalancer_(
          config.at("etl_sources").as_array(),
          flatMapBackend_,
          networkValidatedLedgers_,
          ioc)
{
    flatMapBackend_.open();
    initSchema(pgPool_);
}
