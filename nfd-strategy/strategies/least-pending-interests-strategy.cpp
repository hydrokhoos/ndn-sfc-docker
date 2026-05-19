/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "least-pending-interests-strategy.hpp"

#include "algorithm.hpp"
#include "common/logger.hpp"
#include "daemon/common/global.hpp"

#include <algorithm>
#include <limits>

namespace nfd::fw {

NFD_LOG_INIT(LeastPendingInterestsStrategy);
NFD_REGISTER_STRATEGY(LeastPendingInterestsStrategy);

LeastPendingInterestsStrategy::LeastPendingInterestsStrategy(Forwarder& forwarder,
                                                             const Name& name)
  : Strategy(forwarder)
  , ProcessNackTraits(this)
{
  ParsedInstanceName parsed = parseInstanceName(name);

  if (parsed.version && *parsed.version != getStrategyName()[-1].toVersion()) {
    NDN_THROW(std::invalid_argument(
      "LeastPendingInterestsStrategy does not support version " +
      std::to_string(*parsed.version)));
  }

  StrategyParameters params = parseParameters(parsed.parameters);
  m_retxSuppression = RetxSuppressionExponential::construct(params);

  this->setInstanceName(makeInstanceName(name, getStrategyName()));

  NFD_LOG_DEBUG(*m_retxSuppression);
}

const Name&
LeastPendingInterestsStrategy::getStrategyName()
{
  static const auto strategyName =
    Name("/localhost/nfd/strategy/least-pending-interests").appendVersion(1);
  return strategyName;
}

const fib::NextHop*
LeastPendingInterestsStrategy::chooseBestNextHop(const Name& fibPrefix,
                                                 const Interest& interest,
                                                 const FaceEndpoint& ingress,
                                                 const shared_ptr<pit::Entry>& pitEntry,
                                                 const fib::NextHopList& nexthops,
                                                 bool requireUnused)
{
  const fib::NextHop* best = nullptr;
  size_t bestPending = std::numeric_limits<size_t>::max();
  uint64_t bestCost = std::numeric_limits<uint64_t>::max();
  auto now = time::steady_clock::now();

  auto prefixIt = m_pendingByFibPrefix.find(fibPrefix);

  for (const auto& nexthop : nexthops) {
    bool eligible = requireUnused
      ? isNextHopEligible(ingress.face, interest, nexthop, pitEntry, true, now)
      : isNextHopEligible(ingress.face, interest, nexthop, pitEntry);

    if (!eligible) {
      continue;
    }

    FaceId faceId = nexthop.getFace().getId();
    size_t pending = 0;

    if (prefixIt != m_pendingByFibPrefix.end()) {
      auto faceIt = prefixIt->second.find(faceId);
      if (faceIt != prefixIt->second.end()) {
        pending = faceIt->second;
      }
    }

    // 優先度:
    //  1) pending が小さい
    //  2) 同点なら FIB cost が小さい
    if (best == nullptr ||
        pending < bestPending ||
        (pending == bestPending && nexthop.getCost() < bestCost)) {
      best = &nexthop;
      bestPending = pending;
      bestCost = nexthop.getCost();
    }
  }

  return best;
}

void
LeastPendingInterestsStrategy::decrementPrefixFacePending(const Name& fibPrefix,
                                                          FaceId faceId,
                                                          size_t n)
{
  auto prefixIt = m_pendingByFibPrefix.find(fibPrefix);
  if (prefixIt == m_pendingByFibPrefix.end()) {
    return;
  }

  auto& byFace = prefixIt->second;
  auto faceIt = byFace.find(faceId);
  if (faceIt == byFace.end()) {
    return;
  }

  if (faceIt->second <= n) {
    byFace.erase(faceIt);
  }
  else {
    faceIt->second -= n;
  }

  if (byFace.empty()) {
    m_pendingByFibPrefix.erase(prefixIt);
  }
}

void
LeastPendingInterestsStrategy::trackPending(const pit::Entry* pitEntry,
                                            const Name& fibPrefix,
                                            FaceId faceId,
                                            time::milliseconds timeout)
{
  ++m_pendingByFibPrefix[fibPrefix][faceId];

  auto [pitIt, isNewPit] = m_outstandingByPit.try_emplace(pitEntry);
  PitOutstandingInfo& pitInfo = pitIt->second;
  if (isNewPit) {
    pitInfo.generation = ++m_nextPitGeneration;
  }

  auto& info = pitInfo.byFace[faceId];
  info.fibPrefix = fibPrefix;
  ++info.count;

  // Data/Nack が来ずに timeout になった場合の帳尻合わせ。
  // PIT entry pointer は再利用されうるので generation も照合する。
  uint64_t generation = pitInfo.generation;
  getScheduler().schedule(timeout, [this, pitEntry, faceId, generation] {
    this->finishOnePending(pitEntry, faceId, generation);
  });
}

void
LeastPendingInterestsStrategy::finishOnePending(const pit::Entry* pitEntry, FaceId faceId,
                                                std::optional<uint64_t> generation)
{
  auto pitIt = m_outstandingByPit.find(pitEntry);
  if (pitIt == m_outstandingByPit.end()) {
    return;
  }

  PitOutstandingInfo& pitInfo = pitIt->second;
  if (generation && pitInfo.generation != *generation) {
    return;
  }

  auto& byFace = pitInfo.byFace;
  auto faceIt = byFace.find(faceId);
  if (faceIt == byFace.end()) {
    return;
  }

  OutstandingInfo& info = faceIt->second;
  if (info.count == 0) {
    byFace.erase(faceIt);
    if (byFace.empty()) {
      m_outstandingByPit.erase(pitIt);
    }
    return;
  }

  --info.count;
  decrementPrefixFacePending(info.fibPrefix, faceId, 1);

  if (info.count == 0) {
    byFace.erase(faceIt);
  }
  if (byFace.empty()) {
    m_outstandingByPit.erase(pitIt);
  }
}

void
LeastPendingInterestsStrategy::afterReceiveInterest(const Interest& interest,
                                                    const FaceEndpoint& ingress,
                                                    const shared_ptr<pit::Entry>& pitEntry)
{
  auto suppression = m_retxSuppression->decidePerPitEntry(*pitEntry);

  if (suppression == RetxSuppressionResult::SUPPRESS) {
    // 同じ PIT entry に集約された後続 Interest は追加転送しない
    NFD_LOG_INTEREST_FROM(interest, ingress, "suppressed");
    return;
  }

  const fib::Entry& fibEntry = this->lookupFib(*pitEntry);
  const Name& fibPrefix = fibEntry.getPrefix();
  const fib::NextHopList& nexthops = fibEntry.getNextHops();

  const fib::NextHop* selected = nullptr;

  if (suppression == RetxSuppressionResult::NEW) {
    // 新規 PIT entry: fibPrefix 単位で pending が最小の face を選ぶ
    selected = chooseBestNextHop(fibPrefix, interest, ingress, pitEntry, nexthops, false);
  }
  else {
    // retransmission:
    // まず未使用 upstream の中で LPI
    selected = chooseBestNextHop(fibPrefix, interest, ingress, pitEntry, nexthops, true);

    // なければ eligible 全体の中で LPI
    if (selected == nullptr) {
      selected = chooseBestNextHop(fibPrefix, interest, ingress, pitEntry, nexthops, false);
    }
  }

  if (selected == nullptr) {
    NFD_LOG_INTEREST_FROM(interest, ingress, "no-nexthop");
    lp::NackHeader nackHeader;
    nackHeader.setReason(lp::NackReason::NO_ROUTE);
    this->sendNack(nackHeader, ingress.face, pitEntry);
    this->rejectPendingInterest(pitEntry);
    return;
  }

  Face& outFace = selected->getFace();
  FaceId outFaceId = outFace.getId();

  size_t currentPending = 0;
  auto prefixIt = m_pendingByFibPrefix.find(fibPrefix);
  if (prefixIt != m_pendingByFibPrefix.end()) {
    auto faceIt = prefixIt->second.find(outFaceId);
    if (faceIt != prefixIt->second.end()) {
      currentPending = faceIt->second;
    }
  }

  NFD_LOG_INTEREST_FROM(interest, ingress,
                        "to=" << outFaceId
                              << " fibPrefix=" << fibPrefix
                              << " pending=" << currentPending
                              << " cost=" << selected->getCost());

  pit::OutRecord* outRecord = this->sendInterest(interest, outFace, pitEntry);
  if (outRecord != nullptr) {
    auto timeout = std::min<time::milliseconds>(interest.getInterestLifetime(), 10_days);
    trackPending(pitEntry.get(), fibPrefix, outFaceId, timeout);
  }
}

void
LeastPendingInterestsStrategy::beforeSatisfyInterest(const Data& data,
                                                     const FaceEndpoint& ingress,
                                                     const shared_ptr<pit::Entry>& pitEntry)
{
  NFD_LOG_DEBUG("beforeSatisfyInterest pitEntry=" << pitEntry->getName()
                << " in=" << ingress.face.getId()
                << " data=" << data.getName());

  // Data が返った upstream face の outstanding だけを戻す。
  // retransmission により他 upstream にも送っている場合、それらは Nack または
  // InterestLifetime timeout で戻すことで、処理中の負荷を過小評価しない。
  finishOnePending(pitEntry.get(), ingress.face.getId());
}

void
LeastPendingInterestsStrategy::afterReceiveNack(const lp::Nack& nack,
                                                const FaceEndpoint& ingress,
                                                const shared_ptr<pit::Entry>& pitEntry)
{
  NFD_LOG_NACK_FROM(nack, ingress, "received");

  // Nack を返した upstream 1 本分だけ戻す
  finishOnePending(pitEntry.get(), ingress.face.getId());

  this->processNack(nack, ingress.face, pitEntry);
}

} // namespace nfd::fw
