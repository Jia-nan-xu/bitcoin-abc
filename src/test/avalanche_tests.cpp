// Copyright (c) 2010 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "avalanche.h"
#include "config.h"
#include "net_processing.h" // For PeerLogicValidation

#include "test/test_bitcoin.h"

#include <boost/test/unit_test.hpp>

struct AvalancheTest {
    static void runEventLoop(AvalancheProcessor &p) { p.runEventLoop(); }

    static std::vector<CInv> getInvsForNextPoll(const AvalancheProcessor &p) {
        return p.getInvsForNextPoll();
    }

    static NodeId getSuitableNodeToQuery(AvalancheProcessor &p) {
        return p.getSuitableNodeToQuery();
    }
};

BOOST_FIXTURE_TEST_SUITE(avalanche_tests, TestChain100Setup)

#define REGISTER_VOTE_AND_CHECK(vr, vote, state, finalized, confidence)        \
    vr.registerVote(vote);                                                     \
    BOOST_CHECK_EQUAL(vr.isAccepted(), state);                                 \
    BOOST_CHECK_EQUAL(vr.hasFinalized(), finalized);                           \
    BOOST_CHECK_EQUAL(vr.getConfidence(), confidence);

BOOST_AUTO_TEST_CASE(vote_record) {
    VoteRecord vr;

    // Check initial state.
    BOOST_CHECK_EQUAL(vr.isAccepted(), false);
    BOOST_CHECK_EQUAL(vr.hasFinalized(), false);
    BOOST_CHECK_EQUAL(vr.getConfidence(), 0);

    // We register one vote for, which keep things at 4/4.
    REGISTER_VOTE_AND_CHECK(vr, true, false, false, 0);

    // One more and we are at 5/3.
    REGISTER_VOTE_AND_CHECK(vr, true, false, false, 0);

    // One more and we are at 5/3.
    REGISTER_VOTE_AND_CHECK(vr, true, false, false, 0);

    // One more and we are at 6/2.
    REGISTER_VOTE_AND_CHECK(vr, true, false, false, 0);

    // One more and we are at 6/2.
    REGISTER_VOTE_AND_CHECK(vr, true, false, false, 0);

    // Next vote will flip state, and confidence will increase as long as we
    // vote yes.
    for (int i = 0; i < AVALANCHE_FINALIZATION_SCORE; i++) {
        REGISTER_VOTE_AND_CHECK(vr, true, true, false, i);
    }

    // The next vote will finalize the decision.
    REGISTER_VOTE_AND_CHECK(vr, false, true, true,
                            AVALANCHE_FINALIZATION_SCORE);

    // Now that we have two no votes, confidence stop increasing.
    for (int i = 0; i < 5; i++) {
        REGISTER_VOTE_AND_CHECK(vr, false, true, true,
                                AVALANCHE_FINALIZATION_SCORE);
    }

    // Next vote will flip state, and confidence will increase as long as we
    // vote no.
    for (int i = 0; i < AVALANCHE_FINALIZATION_SCORE; i++) {
        REGISTER_VOTE_AND_CHECK(vr, false, false, false, i);
    }

    // The next vote will finalize the decision.
    REGISTER_VOTE_AND_CHECK(vr, true, false, true,
                            AVALANCHE_FINALIZATION_SCORE);
}

BOOST_AUTO_TEST_CASE(block_update) {
    CBlockIndex index;
    CBlockIndex *pindex = &index;

    std::set<AvalancheBlockUpdate::Status> status{
        AvalancheBlockUpdate::Status::Invalid,
        AvalancheBlockUpdate::Status::Rejected,
        AvalancheBlockUpdate::Status::Accepted,
        AvalancheBlockUpdate::Status::Finalized,
    };

    for (auto s : status) {
        AvalancheBlockUpdate abu(pindex, s);
        BOOST_CHECK(abu.getBlockIndex() == pindex);
        BOOST_CHECK_EQUAL(abu.getStatus(), s);
    }
}

CService ip(uint32_t i) {
    struct in_addr s;
    s.s_addr = i;
    return CService(CNetAddr(s), Params().GetDefaultPort());
}

std::unique_ptr<CNode> ConnectNode(const Config &config, ServiceFlags nServices,
                                   PeerLogicValidation &peerLogic) {
    static NodeId id = 0;

    CAddress addr(ip(GetRandInt(0xffffffff)), NODE_NONE);
    std::unique_ptr<CNode> nodeptr(new CNode(id++, ServiceFlags(NODE_NETWORK),
                                             0, INVALID_SOCKET, addr, 0, 0,
                                             CAddress(), "",
                                             /*fInboundIn=*/false));
    CNode &node = *nodeptr;
    node.SetSendVersion(PROTOCOL_VERSION);
    node.nServices = nServices;
    peerLogic.InitializeNode(config, &node);
    node.nVersion = 1;
    node.fSuccessfullyConnected = true;

    CConnmanTest::AddNode(node);
    return nodeptr;
}

BOOST_AUTO_TEST_CASE(block_register) {
    AvalancheProcessor p(g_connman.get());
    std::vector<AvalancheBlockUpdate> updates;

    CBlock block = CreateAndProcessBlock({}, CScript());
    const uint256 blockHash = block.GetHash();
    const CBlockIndex *pindex = mapBlockIndex[blockHash];

    const Config &config = GetConfig();

    // Create a node that supports avalanche.
    auto avanode = ConnectNode(config, NODE_AVALANCHE, *peerLogic);
    NodeId nodeid = avanode->GetId();

    // Querying for random block returns false.
    BOOST_CHECK(!p.isAccepted(pindex));

    // Add a new block. Check it is added to the polls.
    BOOST_CHECK(p.addBlockToReconcile(pindex));
    auto invs = AvalancheTest::getInvsForNextPoll(p);
    BOOST_CHECK_EQUAL(invs.size(), 1);
    BOOST_CHECK_EQUAL(invs[0].type, MSG_BLOCK);
    BOOST_CHECK(invs[0].hash == blockHash);

    // Newly added blocks are also considered rejected.
    BOOST_CHECK(!p.isAccepted(pindex));

    // Let's vote for this block a few times.
    AvalancheResponse resp{0, {AvalancheVote(0, blockHash)}};
    for (int i = 0; i < 5; i++) {
        AvalancheTest::runEventLoop(p);
        BOOST_CHECK(p.registerVotes(nodeid, resp, updates));
        BOOST_CHECK(!p.isAccepted(pindex));
        BOOST_CHECK_EQUAL(updates.size(), 0);
    }

    // Now the state will flip.
    AvalancheTest::runEventLoop(p);
    BOOST_CHECK(p.registerVotes(nodeid, resp, updates));
    BOOST_CHECK(p.isAccepted(pindex));
    BOOST_CHECK_EQUAL(updates.size(), 1);
    BOOST_CHECK(updates[0].getBlockIndex() == pindex);
    BOOST_CHECK_EQUAL(updates[0].getStatus(),
                      AvalancheBlockUpdate::Status::Accepted);
    updates = {};

    // Now it is accepted, but we can vote for it numerous times.
    for (int i = 1; i < AVALANCHE_FINALIZATION_SCORE; i++) {
        AvalancheTest::runEventLoop(p);
        BOOST_CHECK(p.registerVotes(nodeid, resp, updates));
        BOOST_CHECK(p.isAccepted(pindex));
        BOOST_CHECK_EQUAL(updates.size(), 0);
    }

    // As long as it is not finalized, we poll.
    invs = AvalancheTest::getInvsForNextPoll(p);
    BOOST_CHECK_EQUAL(invs.size(), 1);
    BOOST_CHECK_EQUAL(invs[0].type, MSG_BLOCK);
    BOOST_CHECK(invs[0].hash == blockHash);

    // Now finalize the decision.
    AvalancheTest::runEventLoop(p);
    BOOST_CHECK(p.registerVotes(nodeid, resp, updates));
    BOOST_CHECK_EQUAL(updates.size(), 1);
    BOOST_CHECK(updates[0].getBlockIndex() == pindex);
    BOOST_CHECK_EQUAL(updates[0].getStatus(),
                      AvalancheBlockUpdate::Status::Finalized);
    updates = {};

    // Once the decision is finalized, there is no poll for it.
    invs = AvalancheTest::getInvsForNextPoll(p);
    BOOST_CHECK_EQUAL(invs.size(), 0);

    // Now let's undo this and finalize rejection.
    BOOST_CHECK(p.addBlockToReconcile(pindex));
    invs = AvalancheTest::getInvsForNextPoll(p);
    BOOST_CHECK_EQUAL(invs.size(), 1);
    BOOST_CHECK_EQUAL(invs[0].type, MSG_BLOCK);
    BOOST_CHECK(invs[0].hash == blockHash);

    // Only 3 here as we don't need to flip state.
    resp = {0, {AvalancheVote(1, blockHash)}};
    for (int i = 0; i < 3; i++) {
        AvalancheTest::runEventLoop(p);
        BOOST_CHECK(p.registerVotes(nodeid, resp, updates));
        BOOST_CHECK(!p.isAccepted(pindex));
        BOOST_CHECK_EQUAL(updates.size(), 0);
    }

    // Now it is rejected, but we can vote for it numerous times.
    for (int i = 0; i < AVALANCHE_FINALIZATION_SCORE; i++) {
        AvalancheTest::runEventLoop(p);
        BOOST_CHECK(p.registerVotes(nodeid, resp, updates));
        BOOST_CHECK(!p.isAccepted(pindex));
        BOOST_CHECK_EQUAL(updates.size(), 0);
    }

    // As long as it is not finalized, we poll.
    invs = AvalancheTest::getInvsForNextPoll(p);
    BOOST_CHECK_EQUAL(invs.size(), 1);
    BOOST_CHECK_EQUAL(invs[0].type, MSG_BLOCK);
    BOOST_CHECK(invs[0].hash == blockHash);

    // Now finalize the decision.
    AvalancheTest::runEventLoop(p);
    BOOST_CHECK(p.registerVotes(nodeid, resp, updates));
    BOOST_CHECK(!p.isAccepted(pindex));
    BOOST_CHECK_EQUAL(updates.size(), 1);
    BOOST_CHECK(updates[0].getBlockIndex() == pindex);
    BOOST_CHECK_EQUAL(updates[0].getStatus(),
                      AvalancheBlockUpdate::Status::Invalid);
    updates = {};

    // Once the decision is finalized, there is no poll for it.
    invs = AvalancheTest::getInvsForNextPoll(p);
    BOOST_CHECK_EQUAL(invs.size(), 0);

    // Adding the block twice does nothing.
    BOOST_CHECK(p.addBlockToReconcile(pindex));
    BOOST_CHECK(!p.addBlockToReconcile(pindex));
    BOOST_CHECK(!p.isAccepted(pindex));

    CConnmanTest::ClearNodes();
}

BOOST_AUTO_TEST_CASE(multi_block_register) {
    AvalancheProcessor p(g_connman.get());
    CBlockIndex indexA, indexB;

    std::vector<AvalancheBlockUpdate> updates;

    const Config &config = GetConfig();

    // Create a node that supports avalanche.
    auto node0 = ConnectNode(config, NODE_AVALANCHE, *peerLogic);
    auto node1 = ConnectNode(config, NODE_AVALANCHE, *peerLogic);

    // Make sure the block has a hash.
    CBlock blockA = CreateAndProcessBlock({}, CScript());
    const uint256 blockHashA = blockA.GetHash();
    const CBlockIndex *pindexA = mapBlockIndex[blockHashA];

    CBlock blockB = CreateAndProcessBlock({}, CScript());
    const uint256 blockHashB = blockB.GetHash();
    const CBlockIndex *pindexB = mapBlockIndex[blockHashB];

    // Querying for random block returns false.
    BOOST_CHECK(!p.isAccepted(pindexA));
    BOOST_CHECK(!p.isAccepted(pindexB));

    // Start voting on block A.
    BOOST_CHECK(p.addBlockToReconcile(pindexA));
    auto invs = AvalancheTest::getInvsForNextPoll(p);
    BOOST_CHECK_EQUAL(invs.size(), 1);
    BOOST_CHECK_EQUAL(invs[0].type, MSG_BLOCK);
    BOOST_CHECK(invs[0].hash == blockHashA);

    AvalancheTest::runEventLoop(p);
    BOOST_CHECK(p.registerVotes(node0->GetId(),
                                {0, {AvalancheVote(0, blockHashA)}}, updates));
    BOOST_CHECK_EQUAL(updates.size(), 0);

    // Start voting on block B after one vote.
    AvalancheResponse resp{
        0, {AvalancheVote(0, blockHashB), AvalancheVote(0, blockHashA)}};
    BOOST_CHECK(p.addBlockToReconcile(pindexB));
    invs = AvalancheTest::getInvsForNextPoll(p);
    BOOST_CHECK_EQUAL(invs.size(), 2);

    // Ensure B comes before A because it has accumulated more PoW.
    BOOST_CHECK_EQUAL(invs[0].type, MSG_BLOCK);
    BOOST_CHECK(invs[0].hash == blockHashB);
    BOOST_CHECK_EQUAL(invs[1].type, MSG_BLOCK);
    BOOST_CHECK(invs[1].hash == blockHashA);

    // Let's vote for these blocks a few times.
    for (int i = 0; i < 4; i++) {
        AvalancheTest::runEventLoop(p);
        BOOST_CHECK(p.registerVotes(node0->GetId(), resp, updates));
        BOOST_CHECK_EQUAL(updates.size(), 0);
    }

    // Now the state will flip for A.
    AvalancheTest::runEventLoop(p);
    BOOST_CHECK(p.registerVotes(node0->GetId(), resp, updates));
    BOOST_CHECK_EQUAL(updates.size(), 1);
    BOOST_CHECK(updates[0].getBlockIndex() == pindexA);
    BOOST_CHECK_EQUAL(updates[0].getStatus(),
                      AvalancheBlockUpdate::Status::Accepted);
    updates = {};

    // And then for B.
    AvalancheTest::runEventLoop(p);
    BOOST_CHECK(p.registerVotes(node0->GetId(), resp, updates));
    BOOST_CHECK_EQUAL(updates.size(), 1);
    BOOST_CHECK(updates[0].getBlockIndex() == pindexB);
    BOOST_CHECK_EQUAL(updates[0].getStatus(),
                      AvalancheBlockUpdate::Status::Accepted);
    updates = {};

    // Now it is rejected, but we can vote for it numerous times.
    for (int i = 2; i < AVALANCHE_FINALIZATION_SCORE; i++) {
        AvalancheTest::runEventLoop(p);
        BOOST_CHECK(p.registerVotes(node0->GetId(), resp, updates));
        BOOST_CHECK_EQUAL(updates.size(), 0);
    }

    // Running two iterration of the event loop so that vote gets triggerd on A
    // and B.
    AvalancheTest::runEventLoop(p);
    AvalancheTest::runEventLoop(p);

    // Next vote will finalize block A.
    BOOST_CHECK(p.registerVotes(node1->GetId(), resp, updates));
    BOOST_CHECK_EQUAL(updates.size(), 1);
    BOOST_CHECK(updates[0].getBlockIndex() == pindexA);
    BOOST_CHECK_EQUAL(updates[0].getStatus(),
                      AvalancheBlockUpdate::Status::Finalized);
    updates = {};

    // We do not vote on A anymore.
    invs = AvalancheTest::getInvsForNextPoll(p);
    BOOST_CHECK_EQUAL(invs.size(), 1);
    BOOST_CHECK_EQUAL(invs[0].type, MSG_BLOCK);
    BOOST_CHECK(invs[0].hash == blockHashB);

    // Next vote will finalize block B.
    BOOST_CHECK(p.registerVotes(node0->GetId(), resp, updates));
    BOOST_CHECK_EQUAL(updates.size(), 1);
    BOOST_CHECK(updates[0].getBlockIndex() == pindexB);
    BOOST_CHECK_EQUAL(updates[0].getStatus(),
                      AvalancheBlockUpdate::Status::Finalized);
    updates = {};

    // There is nothing left to vote on.
    invs = AvalancheTest::getInvsForNextPoll(p);
    BOOST_CHECK_EQUAL(invs.size(), 0);

    CConnmanTest::ClearNodes();
}

BOOST_AUTO_TEST_CASE(poll_and_response) {
    AvalancheProcessor p(g_connman.get());

    std::vector<AvalancheBlockUpdate> updates;

    CBlock block = CreateAndProcessBlock({}, CScript());
    const uint256 blockHash = block.GetHash();
    const CBlockIndex *pindex = mapBlockIndex[blockHash];

    const Config &config = GetConfig();

    // There is no node to query.
    BOOST_CHECK_EQUAL(AvalancheTest::getSuitableNodeToQuery(p), -1);

    // Create a node that supports avalanche and one that doesn't.
    auto oldnode = ConnectNode(config, NODE_NONE, *peerLogic);
    auto avanode = ConnectNode(config, NODE_AVALANCHE, *peerLogic);
    NodeId avanodeid = avanode->GetId();

    // It returns the avalanche peer.
    BOOST_CHECK_EQUAL(AvalancheTest::getSuitableNodeToQuery(p), avanodeid);

    // Register a block and check it is added to the list of elements to poll.
    BOOST_CHECK(p.addBlockToReconcile(pindex));
    auto invs = AvalancheTest::getInvsForNextPoll(p);
    BOOST_CHECK_EQUAL(invs.size(), 1);
    BOOST_CHECK_EQUAL(invs[0].type, MSG_BLOCK);
    BOOST_CHECK(invs[0].hash == blockHash);

    // Trigger a poll on avanode.
    AvalancheTest::runEventLoop(p);

    // There is no more suitable peer available, so return nothing.
    BOOST_CHECK_EQUAL(AvalancheTest::getSuitableNodeToQuery(p), -1);

    // Respond to the request.
    AvalancheResponse resp = {0, {AvalancheVote(0, blockHash)}};
    BOOST_CHECK(p.registerVotes(avanodeid, resp, updates));
    BOOST_CHECK_EQUAL(updates.size(), 0);

    // Now that avanode fullfilled his request, it is added back to the list of
    // queriable nodes.
    BOOST_CHECK_EQUAL(AvalancheTest::getSuitableNodeToQuery(p), avanodeid);

    // Sending a response when not polled fails.
    BOOST_CHECK(!p.registerVotes(avanodeid, resp, updates));
    BOOST_CHECK_EQUAL(updates.size(), 0);

    // Trigger a poll on avanode.
    AvalancheTest::runEventLoop(p);
    BOOST_CHECK_EQUAL(AvalancheTest::getSuitableNodeToQuery(p), -1);

    // Sending responses that do not match the request also fails.
    // 1. Too many results.
    resp = {0, {AvalancheVote(0, blockHash), AvalancheVote(0, blockHash)}};
    AvalancheTest::runEventLoop(p);
    BOOST_CHECK(!p.registerVotes(avanodeid, resp, updates));
    BOOST_CHECK_EQUAL(updates.size(), 0);
    BOOST_CHECK_EQUAL(AvalancheTest::getSuitableNodeToQuery(p), avanodeid);

    // 2. Not enough results.
    resp = {0, {}};
    AvalancheTest::runEventLoop(p);
    BOOST_CHECK(!p.registerVotes(avanodeid, resp, updates));
    BOOST_CHECK_EQUAL(updates.size(), 0);
    BOOST_CHECK_EQUAL(AvalancheTest::getSuitableNodeToQuery(p), avanodeid);

    // 3. Do not match the poll.
    resp = {0, {AvalancheVote()}};
    AvalancheTest::runEventLoop(p);
    BOOST_CHECK(!p.registerVotes(avanodeid, resp, updates));
    BOOST_CHECK_EQUAL(updates.size(), 0);
    BOOST_CHECK_EQUAL(AvalancheTest::getSuitableNodeToQuery(p), avanodeid);

    // Proper response gets processed and avanode is available again.
    resp = {0, {AvalancheVote(0, blockHash)}};
    AvalancheTest::runEventLoop(p);
    BOOST_CHECK(p.registerVotes(avanodeid, resp, updates));
    BOOST_CHECK_EQUAL(updates.size(), 0);
    BOOST_CHECK_EQUAL(AvalancheTest::getSuitableNodeToQuery(p), avanodeid);

    // Making request for invalid nodes do not work.
    BOOST_CHECK(!p.registerVotes(avanodeid + 1234, resp, updates));
    BOOST_CHECK_EQUAL(updates.size(), 0);

    // Out of order response are rejected.
    CBlock block2 = CreateAndProcessBlock({}, CScript());
    const uint256 blockHash2 = block2.GetHash();
    const CBlockIndex *pindex2 = mapBlockIndex[blockHash2];
    BOOST_CHECK(p.addBlockToReconcile(pindex2));

    resp = {0, {AvalancheVote(0, blockHash), AvalancheVote(0, blockHash2)}};
    AvalancheTest::runEventLoop(p);
    BOOST_CHECK(!p.registerVotes(avanodeid, resp, updates));
    BOOST_CHECK_EQUAL(updates.size(), 0);
    BOOST_CHECK_EQUAL(AvalancheTest::getSuitableNodeToQuery(p), avanodeid);

    CConnmanTest::ClearNodes();
}

BOOST_AUTO_TEST_CASE(event_loop) {
    AvalancheProcessor p(g_connman.get());
    CScheduler s;

    CBlock block = CreateAndProcessBlock({}, CScript());
    const uint256 blockHash = block.GetHash();
    const CBlockIndex *pindex = mapBlockIndex[blockHash];

    // Starting the event loop.
    BOOST_CHECK(p.startEventLoop(s));

    // There is one task planned in the next hour (our event loop).
    boost::chrono::system_clock::time_point start, stop;
    BOOST_CHECK_EQUAL(s.getQueueInfo(start, stop), 1);

    // Starting twice doesn't start it twice.
    BOOST_CHECK(!p.startEventLoop(s));

    // Start the scheduler thread.
    std::thread schedulerThread(std::bind(&CScheduler::serviceQueue, &s));

    // Create a node and a block to query.
    const Config &config = GetConfig();

    // Create a node that supports avalanche.
    auto avanode = ConnectNode(config, NODE_AVALANCHE, *peerLogic);
    NodeId nodeid = avanode->GetId();

    // There is no query in flight at the moment.
    BOOST_CHECK_EQUAL(AvalancheTest::getSuitableNodeToQuery(p), nodeid);

    // Add a new block. Check it is added to the polls.
    BOOST_CHECK(p.addBlockToReconcile(pindex));

    bool hasQueried = false;
    for (int i = 0; i < 1000; i++) {
        // Technically, this is a race condition, but this should do just fine
        // as we wait up to 1s for an event that should take 10ms.
        boost::this_thread::sleep_for(boost::chrono::milliseconds(1));
        if (AvalancheTest::getSuitableNodeToQuery(p) == -1) {
            hasQueried = true;
            break;
        }
    }

    BOOST_CHECK(hasQueried);

    // Stop event loop.
    BOOST_CHECK(p.stopEventLoop());

    // We don't have any task scheduled anymore.
    BOOST_CHECK_EQUAL(s.getQueueInfo(start, stop), 0);

    // Can't stop the event loop twice.
    BOOST_CHECK(!p.stopEventLoop());

    // Wait for the scheduler to stop.
    s.stop(true);
    schedulerThread.join();

    CConnmanTest::ClearNodes();
}

BOOST_AUTO_TEST_CASE(destructor) {
    CScheduler s;
    boost::chrono::system_clock::time_point start, stop;

    // Start the scheduler thread.
    std::thread schedulerThread(std::bind(&CScheduler::serviceQueue, &s));

    {
        AvalancheProcessor p(g_connman.get());
        BOOST_CHECK(p.startEventLoop(s));
        BOOST_CHECK_EQUAL(s.getQueueInfo(start, stop), 1);
    }

    // Now that avalanche is destroyed, there is no more scheduled tasks.
    BOOST_CHECK_EQUAL(s.getQueueInfo(start, stop), 0);

    // Wait for the scheduler to stop.
    s.stop(true);
    schedulerThread.join();
}

BOOST_AUTO_TEST_SUITE_END()
