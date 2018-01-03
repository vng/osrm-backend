#include "util/typedefs.hpp"

#include <unordered_set>

namespace osrm
{
namespace util
{
class NameTable;
}

namespace extractor
{
class NodeBasedGraphFactory;

namespace guidance
{
// Find all "segregated" edges, e.g. edges that can be skipped in turn instructions.
// The main cases are:
// - middle edges between two osm ways in one logic road (U-turn)
// - staggered intersections (X-cross)
// - square/circle intersections
std::unordered_set<EdgeID> findSegregatedNodes(const NodeBasedGraphFactory &factory,
                                               const util::NameTable &names);

void FindTopInterestngCrossings(const NodeBasedGraphFactory &factory, size_t count);

}
}
}
