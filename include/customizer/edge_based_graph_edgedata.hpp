#ifndef OSRM_CUSTOMIZE_EDGE_BASED_GRAPH_EDGEDATA_HPP
#define OSRM_CUSTOMIZE_EDGE_BASED_GRAPH_EDGEDATA_HPP

#include "util/typedefs.hpp"

namespace osrm::customizer
{

struct EdgeBasedGraphEdgeData
{
    NodeID turn_id; // ID of the edge based node (node based edge)
};

} // namespace osrm::customizer

#endif // OSRM_CUSTOMIZE_EDGE_BASED_GRAPH_EDGEDATA_HPP
