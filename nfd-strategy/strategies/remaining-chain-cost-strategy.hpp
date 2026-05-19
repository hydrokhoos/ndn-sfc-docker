/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#ifndef NFD_DAEMON_FW_REMAINING_CHAIN_COST_STRATEGY_HPP
#define NFD_DAEMON_FW_REMAINING_CHAIN_COST_STRATEGY_HPP

#include "strategy.hpp"
#include "process-nack-traits.hpp"
#include "retx-suppression-exponential.hpp"

#include <vector>

namespace nfd::fw {

class RemainingChainCostStrategy : public Strategy
                                 , public ProcessNackTraits<RemainingChainCostStrategy>
{
public:
  explicit
  RemainingChainCostStrategy(Forwarder& forwarder,
                             const Name& name = getStrategyName());

  static const Name&
  getStrategyName();

public: // triggers
  void
  afterReceiveInterest(const Interest& interest, const FaceEndpoint& ingress,
                       const shared_ptr<pit::Entry>& pitEntry) override;

  void
  afterReceiveNack(const lp::Nack& nack, const FaceEndpoint& ingress,
                   const shared_ptr<pit::Entry>& pitEntry) override;

private:
  static constexpr double INF_COST = 1e9;

  bool
  isSfcInterest(const Name& name) const;

  std::vector<Name>
  parseRemainingTargets(const Name& name) const;

  double
  getWeight(size_t index, bool isContent) const;

  const fib::Entry&
  lookupFibEntry(const Name& target) const;

  double
  getCostViaFace(const Name& target, const Face& face) const;

  double
  computeScore(const Face& face, const std::vector<Name>& targets) const;

  const fib::NextHop*
  selectLocalNextHop(const Interest& interest,
                     const FaceEndpoint& ingress,
                     const shared_ptr<pit::Entry>& pitEntry,
                     const fib::NextHopList& nexthops,
                     bool requireUnused) const;

  const fib::NextHop*
  selectBestFaceByRemainingChainCost(const Interest& interest,
                                     const FaceEndpoint& ingress,
                                     const shared_ptr<pit::Entry>& pitEntry,
                                     const fib::NextHopList& candidateNextHops,
                                     const std::vector<Name>& targets,
                                     bool requireUnused) const;

  void
  fallbackToBestRoute(const Interest& interest, const FaceEndpoint& ingress,
                      const shared_ptr<pit::Entry>& pitEntry,
                      RetxSuppressionResult suppression);

private:
  const Fib& m_fib;
  std::unique_ptr<RetxSuppressionExponential> m_retxSuppression;

  friend ProcessNackTraits<RemainingChainCostStrategy>;
};

} // namespace nfd::fw

#endif // NFD_DAEMON_FW_REMAINING_CHAIN_COST_STRATEGY_HPP
