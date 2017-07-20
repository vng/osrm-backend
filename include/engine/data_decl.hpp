#pragma once

#include "util/integer_range.hpp"
#include "util/typedefs.hpp"

namespace osrm
{
namespace engine
{

struct MultiLayerDijkstraHeapData
{
    NodeID parent;
    bool from_clique_arc;
    MultiLayerDijkstraHeapData(NodeID p) : parent(p), from_clique_arc(false) {}
    MultiLayerDijkstraHeapData(NodeID p, bool from) : parent(p), from_clique_arc(from) {}
};

struct ManyToManyMultiLayerDijkstraHeapData : MultiLayerDijkstraHeapData
{
    EdgeWeight duration;
    ManyToManyMultiLayerDijkstraHeapData(NodeID p, EdgeWeight duration)
        : MultiLayerDijkstraHeapData(p), duration(duration)
    {
    }
    ManyToManyMultiLayerDijkstraHeapData(NodeID p, bool from, EdgeWeight duration)
        : MultiLayerDijkstraHeapData(p, from), duration(duration)
    {
    }
};

template <typename AlgorithmT> struct SearchEngineData;

namespace datafacade
{

using EdgeRange = util::range<EdgeID>;

template <typename AlgorithmT> class ContiguousInternalMemoryDataFacade;

}

}
}
