FROM hydrokhoos/ndn-all:latest

COPY nfd-strategy/strategies/least-pending-interests-strategy.hpp /NFD/daemon/fw/
COPY nfd-strategy/strategies/least-pending-interests-strategy.cpp /NFD/daemon/fw/
COPY nfd-strategy/strategies/remaining-chain-cost-strategy.hpp /NFD/daemon/fw/
COPY nfd-strategy/strategies/remaining-chain-cost-strategy.cpp /NFD/daemon/fw/

RUN cd /NFD \
    && ./waf configure \
    && ./waf build -j2 \
    && ./waf install
