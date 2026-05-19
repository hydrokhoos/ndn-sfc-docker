/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#ifndef NFD_DAEMON_FW_LEAST_PENDING_INTERESTS_STRATEGY_HPP
#define NFD_DAEMON_FW_LEAST_PENDING_INTERESTS_STRATEGY_HPP

#include "strategy.hpp"
#include "process-nack-traits.hpp"
#include "retx-suppression-exponential.hpp"

#include <cstdint>
#include <cstddef>
#include <map>
#include <optional>
#include <unordered_map>

namespace nfd::fw {

class LeastPendingInterestsStrategy : public Strategy
                                    , public ProcessNackTraits<LeastPendingInterestsStrategy>
{
public:
  explicit
  LeastPendingInterestsStrategy(Forwarder& forwarder,
                                const Name& name = getStrategyName());

  static const Name&
  getStrategyName();

public: // triggers
  void
  afterReceiveInterest(const Interest& interest, const FaceEndpoint& ingress,
                       const shared_ptr<pit::Entry>& pitEntry) override;

  void
  beforeSatisfyInterest(const Data& data, const FaceEndpoint& ingress,
                        const shared_ptr<pit::Entry>& pitEntry) override;

  void
  afterReceiveNack(const lp::Nack& nack, const FaceEndpoint& ingress,
                   const shared_ptr<pit::Entry>& pitEntry) override;

private:
  struct OutstandingInfo
  {
    Name fibPrefix;
    size_t count = 0;
  };

  struct PitOutstandingInfo
  {
    uint64_t generation = 0;
    std::unordered_map<FaceId, OutstandingInfo> byFace;
  };

private:
  const fib::NextHop*
  chooseBestNextHop(const Name& fibPrefix,
                    const Interest& interest,
                    const FaceEndpoint& ingress,
                    const shared_ptr<pit::Entry>& pitEntry,
                    const fib::NextHopList& nexthops,
                    bool requireUnused);

  void
  trackPending(const pit::Entry* pitEntry, const Name& fibPrefix, FaceId faceId,
               time::milliseconds timeout);

  void
  finishOnePending(const pit::Entry* pitEntry, FaceId faceId,
                   std::optional<uint64_t> generation = std::nullopt);

  void
  decrementPrefixFacePending(const Name& fibPrefix, FaceId faceId, size_t n = 1);

private:
  std::unique_ptr<RetxSuppressionExponential> m_retxSuppression;

  // FIB 最長一致 prefix ごとの face別 pending 数
  // key: fibPrefix -> (faceId -> pending count)
  std::map<Name, std::unordered_map<FaceId, size_t>> m_pendingByFibPrefix;

  // PIT entry ごとに、どの fibPrefix / faceId に何本 outstanding を持っているか
  // key は PIT entry pointer だが、timer が古い PIT と新しい PIT を誤認しないよう
  // generation も照合する。
  std::unordered_map<const pit::Entry*, PitOutstandingInfo> m_outstandingByPit;

  uint64_t m_nextPitGeneration = 0;

  friend ProcessNackTraits<LeastPendingInterestsStrategy>;
};

} // namespace nfd::fw

#endif // NFD_DAEMON_FW_LEAST_PENDING_INTERESTS_STRATEGY_HPP
