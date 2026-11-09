#include "../../native/capacity_policy.h"
#include <cassert>
using s2a::CpuCapacity;
int main() {
    using s2a::highest_capacity_cpus;
    assert(highest_capacity_cpus({}).empty());
    assert(highest_capacity_cpus({{2,512},{7,0}}).empty());
    assert(highest_capacity_cpus({{2,512},{7,512}}).empty());
    assert((highest_capacity_cpus({{1,700},{6,1000},{7,1000},{18,100}})==std::vector<unsigned>{6,7}));
    assert((highest_capacity_cpus({{123,1024},{2,128},{88,1024}})==std::vector<unsigned>{123,88}));
    using s2a::capacity_cpus;
    const std::vector<CpuCapacity> three{{0,256},{1,512},{6,1024},{7,1024}};
    assert((capacity_cpus(three,"split",0)==std::vector<unsigned>{7}));
    assert((capacity_cpus(three,"split",1)==std::vector<unsigned>{6}));
    assert((capacity_cpus(three,"split2",1)==std::vector<unsigned>{6}));
    // Actual 9000 topology: only CPU7 is in the maximum-capacity group.
    const std::vector<CpuCapacity> d9000{{0,200},{1,200},{2,200},{3,200},{4,838},{5,838},{6,838},{7,1024}};
    assert(capacity_cpus(d9000,"split",0).empty());
    assert(capacity_cpus(d9000,"split",1).empty());
    assert(capacity_cpus(d9000,"split2",0).empty());
    // If the allowed mask excludes CPU7, select only from its new maximum group.
    const std::vector<CpuCapacity> restricted{{0,200},{4,838},{5,838},{6,838}};
    assert((capacity_cpus(restricted,"split",0)==std::vector<unsigned>{6}));
    assert((capacity_cpus(restricted,"split",1)==std::vector<unsigned>{5}));
    // Actual 8E topology: two CPUs share the maximum capacity.
    const std::vector<CpuCapacity> elite{{0,792},{1,792},{2,792},{3,792},{4,792},{5,792},{6,1024},{7,1024}};
    assert((capacity_cpus(elite,"split",0)==std::vector<unsigned>{7}));
    assert((capacity_cpus(elite,"split",1)==std::vector<unsigned>{6}));
    assert((capacity_cpus(elite,"split2",0)==std::vector<unsigned>{7}));
    assert((capacity_cpus(elite,"split2",1)==std::vector<unsigned>{6}));
    assert((capacity_cpus(three,"top2grp",0)==std::vector<unsigned>{7,6,1}));
    assert(capacity_cpus({{0,512}},"split",0).empty());
    assert(capacity_cpus({{0,0},{1,512}},"split",0).empty());
    assert(capacity_cpus(three,"bad",0).empty());
}
