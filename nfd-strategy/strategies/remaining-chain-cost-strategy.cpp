/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "remaining-chain-cost-strategy.hpp"

#include "algorithm.hpp"
#include "common/logger.hpp"

#include <algorithm>
#include <limits>
#include <sstream>

namespace nfd::fw {

NFD_LOG_INIT(RemainingChainCostStrategy);
NFD_REGISTER_STRATEGY(RemainingChainCostStrategy);

RemainingChainCostStrategy::RemainingChainCostStrategy(Forwarder& forwarder,
                                                       const Name& name)
  : Strategy(forwarder)
  , ProcessNackTraits(this)
  , m_fib(forwarder.getFib())
{
  ParsedInstanceName parsed = parseInstanceName(name);

  if (parsed.version && *parsed.version != getStrategyName()[-1].toVersion()) {
    NDN_THROW(std::invalid_argument(
      "RemainingChainCostStrategy does not support version " +
      std::to_string(*parsed.version)));
  }

  StrategyParameters params = parseParameters(parsed.parameters);
  m_retxSuppression = RetxSuppressionExponential::construct(params);

  this->setInstanceName(makeInstanceName(name, getStrategyName()));

  NFD_LOG_DEBUG(*m_retxSuppression);
}

const Name&
RemainingChainCostStrategy::getStrategyName()
{
  static const auto strategyName =
    Name("/localhost/nfd/strategy/remaining-chain-cost").appendVersion(1);
  return strategyName;
}

bool
RemainingChainCostStrategy::isSfcInterest(const Name& name) const
{
  return name.size() >= 1 && name.at(0).toUri() == "sfc";
}

std::vector<Name>
RemainingChainCostStrategy::parseRemainingTargets(const Name& name) const
{
  std::vector<Name> targets;

  if (!isSfcInterest(name) || name.size() < 3) {
    return targets;
  }

  for (size_t i = 1; i + 1 < name.size(); ++i) {
    targets.push_back(Name("/svc").append(name.at(i)));
  }

  targets.push_back(Name("/content").append(name.at(-1)));
  return targets;
}

double
RemainingChainCostStrategy::getWeight(size_t index, bool isContent) const
{
  if (isContent) {
    return 0.3;
  }

  return index == 0 ? 1.0 : 0.5;
}

const fib::Entry&
RemainingChainCostStrategy::lookupFibEntry(const Name& target) const
{
  return m_fib.findLongestPrefixMatch(target);
}

double
RemainingChainCostStrategy::getCostViaFace(const Name& target, const Face& face) const
{
  const fib::Entry& fibEntry = lookupFibEntry(target);

  for (const auto& nexthop : fibEntry.getNextHops()) {
    if (nexthop.getFace().getId() == face.getId()) {
      return static_cast<double>(nexthop.getCost());
    }
  }

  return INF_COST;
}

double
RemainingChainCostStrategy::computeScore(const Face& face,
                                         const std::vector<Name>& targets) const
{
  double score = 0.0;

  for (size_t i = 0; i < targets.size(); ++i) {
    bool isContent = i + 1 == targets.size();
    double cost = getCostViaFace(targets[i], face);
    if (cost >= INF_COST) {
      return INF_COST;
    }

    score += getWeight(i, isContent) * cost;
  }

  return score;
}

const fib::NextHop*
RemainingChainCostStrategy::selectLocalNextHop(const Interest& interest,
                                               const FaceEndpoint& ingress,
                                               const shared_ptr<pit::Entry>& pitEntry,
                                               const fib::NextHopList& nexthops,
                                               bool requireUnused) const
{
  const fib::NextHop* best = nullptr;
  uint64_t bestCost = std::numeric_limits<uint64_t>::max();
  FaceId bestFaceId = std::numeric_limits<FaceId>::max();
  auto now = time::steady_clock::now();

  for (const auto& nexthop : nexthops) {
    Face& face = nexthop.getFace();
    if (face.getScope() != ndn::nfd::FACE_SCOPE_LOCAL) {
      continue;
    }

    bool eligible = requireUnused
      ? isNextHopEligible(ingress.face, interest, nexthop, pitEntry, true, now)
      : isNextHopEligible(ingress.face, interest, nexthop, pitEntry);

    if (!eligible) {
      continue;
    }

    FaceId faceId = face.getId();
    if (best == nullptr ||
        nexthop.getCost() < bestCost ||
        (nexthop.getCost() == bestCost && faceId < bestFaceId)) {
      best = &nexthop;
      bestCost = nexthop.getCost();
      bestFaceId = faceId;
    }
  }

  return best;
}

const fib::NextHop*
RemainingChainCostStrategy::selectBestFaceByRemainingChainCost(
  const Interest& interest,
  const FaceEndpoint& ingress,
  const shared_ptr<pit::Entry>& pitEntry,
  const fib::NextHopList& candidateNextHops,
  const std::vector<Name>& targets,
  bool requireUnused) const
{
  const fib::NextHop* best = nullptr;
  double bestScore = INF_COST;
  double bestFirstCost = INF_COST;
  FaceId bestFaceId = std::numeric_limits<FaceId>::max();
  auto now = time::steady_clock::now();

  for (const auto& nexthop : candidateNextHops) {
    bool eligible = requireUnused
      ? isNextHopEligible(ingress.face, interest, nexthop, pitEntry, true, now)
      : isNextHopEligible(ingress.face, interest, nexthop, pitEntry);

    if (!eligible) {
      continue;
    }

    const Face& face = nexthop.getFace();
    double firstCost = getCostViaFace(targets.front(), face);
    double score = computeScore(face, targets);

    std::ostringstream os;
    os << "remaining-chain-cost face=" << face.getId();
    for (const auto& target : targets) {
      double cost = getCostViaFace(target, face);
      os << " cost(" << target << ")=";
      if (cost >= INF_COST) {
        os << "INF";
      }
      else {
        os << cost;
      }
    }
    os << " score=";
    if (score >= INF_COST) {
      os << "INF";
    }
    else {
      os << score;
    }
    NFD_LOG_TRACE(os.str());

    if (score >= INF_COST) {
      continue;
    }

    FaceId faceId = face.getId();
    if (best == nullptr ||
        score < bestScore ||
        (score == bestScore && firstCost < bestFirstCost) ||
        (score == bestScore && firstCost == bestFirstCost && faceId < bestFaceId)) {
      best = &nexthop;
      bestScore = score;
      bestFirstCost = firstCost;
      bestFaceId = faceId;
    }
  }

  return best;
}

void
RemainingChainCostStrategy::fallbackToBestRoute(const Interest& interest,
                                                const FaceEndpoint& ingress,
                                                const shared_ptr<pit::Entry>& pitEntry,
                                                RetxSuppressionResult suppression)
{
  const fib::Entry& fibEntry = this->lookupFib(*pitEntry);
  const fib::NextHopList& nexthops = fibEntry.getNextHops();
  auto it = nexthops.end();

  if (suppression == RetxSuppressionResult::NEW) {
    it = std::find_if(nexthops.begin(), nexthops.end(), [&] (const auto& nexthop) {
      return isNextHopEligible(ingress.face, interest, nexthop, pitEntry);
    });

    if (it == nexthops.end()) {
      NFD_LOG_INTEREST_FROM(interest, ingress, "remaining-chain-cost fallback new no-nexthop");
      lp::NackHeader nackHeader;
      nackHeader.setReason(lp::NackReason::NO_ROUTE);
      this->sendNack(nackHeader, ingress.face, pitEntry);
      this->rejectPendingInterest(pitEntry);
      return;
    }

    Face& outFace = it->getFace();
    NFD_LOG_INTEREST_FROM(interest, ingress,
                          "remaining-chain-cost fallback new to=" << outFace.getId());
    this->sendInterest(interest, outFace, pitEntry);
    return;
  }

  it = std::find_if(nexthops.begin(), nexthops.end(),
                    [&, now = time::steady_clock::now()] (const auto& nexthop) {
                      return isNextHopEligible(ingress.face, interest, nexthop, pitEntry, true, now);
                    });

  if (it != nexthops.end()) {
    Face& outFace = it->getFace();
    this->sendInterest(interest, outFace, pitEntry);
    NFD_LOG_INTEREST_FROM(interest, ingress,
                          "remaining-chain-cost fallback retx unused-to=" << outFace.getId());
    return;
  }

  it = findEligibleNextHopWithEarliestOutRecord(ingress.face, interest, nexthops, pitEntry);
  if (it == nexthops.end()) {
    NFD_LOG_INTEREST_FROM(interest, ingress, "remaining-chain-cost fallback retx no-nexthop");
  }
  else {
    Face& outFace = it->getFace();
    this->sendInterest(interest, outFace, pitEntry);
    NFD_LOG_INTEREST_FROM(interest, ingress,
                          "remaining-chain-cost fallback retx retry-to=" << outFace.getId());
  }
}

void
RemainingChainCostStrategy::afterReceiveInterest(const Interest& interest,
                                                 const FaceEndpoint& ingress,
                                                 const shared_ptr<pit::Entry>& pitEntry)
{
  auto suppression = m_retxSuppression->decidePerPitEntry(*pitEntry);

  if (suppression == RetxSuppressionResult::SUPPRESS) {
    NFD_LOG_INTEREST_FROM(interest, ingress, "suppressed");
    return;
  }

  const Name& interestName = interest.getName();
  if (!isSfcInterest(interestName)) {
    NFD_LOG_TRACE("remaining-chain-cost fallback non-sfc interest=" << interestName);
    fallbackToBestRoute(interest, ingress, pitEntry, suppression);
    return;
  }

  const fib::Entry& originalFibEntry = this->lookupFib(*pitEntry);
  const fib::NextHop* localNextHop = nullptr;
  if (suppression == RetxSuppressionResult::NEW) {
    localNextHop = selectLocalNextHop(interest, ingress, pitEntry,
                                      originalFibEntry.getNextHops(), false);
  }
  else {
    localNextHop = selectLocalNextHop(interest, ingress, pitEntry,
                                      originalFibEntry.getNextHops(), true);
    if (localNextHop == nullptr) {
      localNextHop = selectLocalNextHop(interest, ingress, pitEntry,
                                        originalFibEntry.getNextHops(), false);
    }
  }

  if (localNextHop != nullptr) {
    Face& outFace = localNextHop->getFace();
    NFD_LOG_TRACE("remaining-chain-cost local-service face=" << outFace.getId()
                  << " fibPrefix=" << originalFibEntry.getPrefix());
    NFD_LOG_INTEREST_FROM(interest, ingress,
                          "remaining-chain-cost local-service to=" << outFace.getId()
                                                                   << " fibPrefix=" << originalFibEntry.getPrefix());
    this->sendInterest(interest, outFace, pitEntry);
    return;
  }

  std::vector<Name> targets = parseRemainingTargets(interestName);
  if (targets.empty()) {
    NFD_LOG_TRACE("remaining-chain-cost fallback malformed-sfc interest=" << interestName);
    fallbackToBestRoute(interest, ingress, pitEntry, suppression);
    return;
  }

  NFD_LOG_TRACE("remaining-chain-cost interest=" << interestName);
  {
    std::ostringstream os;
    os << "remaining-chain-cost";
    for (size_t i = 0; i < targets.size(); ++i) {
      os << " target[" << i << "]=" << targets[i];
    }
    NFD_LOG_TRACE(os.str());
  }

  const fib::Entry& nextServiceFibEntry = lookupFibEntry(targets.front());
  if (!nextServiceFibEntry.hasNextHops()) {
    NFD_LOG_TRACE("remaining-chain-cost fallback no-next-service interest=" << interestName
                  << " nextService=" << targets.front());
    fallbackToBestRoute(interest, ingress, pitEntry, suppression);
    return;
  }

  const fib::NextHop* selected = nullptr;
  if (suppression == RetxSuppressionResult::NEW) {
    selected = selectBestFaceByRemainingChainCost(interest, ingress, pitEntry,
                                                  nextServiceFibEntry.getNextHops(),
                                                  targets, false);
  }
  else {
    selected = selectBestFaceByRemainingChainCost(interest, ingress, pitEntry,
                                                  nextServiceFibEntry.getNextHops(),
                                                  targets, true);
    if (selected == nullptr) {
      selected = selectBestFaceByRemainingChainCost(interest, ingress, pitEntry,
                                                    nextServiceFibEntry.getNextHops(),
                                                    targets, false);
    }
  }

  if (selected == nullptr) {
    NFD_LOG_TRACE("remaining-chain-cost fallback no-valid-face interest=" << interestName);
    fallbackToBestRoute(interest, ingress, pitEntry, suppression);
    return;
  }

  Face& outFace = selected->getFace();
  NFD_LOG_TRACE("remaining-chain-cost selected face=" << outFace.getId());
  NFD_LOG_INTEREST_FROM(interest, ingress,
                        "remaining-chain-cost to=" << outFace.getId()
                                                   << " nextService=" << targets.front());
  this->sendInterest(interest, outFace, pitEntry);
}

void
RemainingChainCostStrategy::afterReceiveNack(const lp::Nack& nack,
                                             const FaceEndpoint& ingress,
                                             const shared_ptr<pit::Entry>& pitEntry)
{
  this->processNack(nack, ingress.face, pitEntry);
}

} // namespace nfd::fw
