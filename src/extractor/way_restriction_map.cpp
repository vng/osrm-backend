#include "extractor/way_restriction_map.hpp"

#include <iterator>

namespace osrm
{
namespace extractor
{

WayRestrictionMap::WayRestrictionMap(const std::vector<TurnRestriction> &turn_restrictions)
{
    // map all way restrictions into access containers
    const auto prepare_way_restriction = [this](const auto &restriction) {
        if (restriction.Type() != RestrictionType::WAY_RESTRICTION)
            return;

        const auto &way = restriction.AsWayRestriction();
        restriction_starts.insert(
            std::make_pair(std::make_pair(way.in_restriction.via, way.in_restriction.to),
                           restriction_data.size()));
        restriction_ends.insert(
            std::make_pair(std::make_pair(way.out_restriction.from, way.out_restriction.via),
                           restriction_data.size()));
        via_ways.insert(
            std::make_pair(std::make_pair(way.in_restriction.via, way.out_restriction.via),
                           restriction_data.size()));

        restriction_data.push_back(restriction);
    };
    std::for_each(turn_restrictions.begin(), turn_restrictions.end(), prepare_way_restriction);
}

// check if an edge between two nodes is a restricted turn
bool WayRestrictionMap::IsStart(const NodeID from, const NodeID to) const
{
    return restriction_starts.count(std::make_pair(from, to)) > 0;
}
bool WayRestrictionMap::IsEnd(const NodeID from, const NodeID to) const
{
    return restriction_ends.count(std::make_pair(from, to)) > 0;
}
bool WayRestrictionMap::IsViaWay(const NodeID from, const NodeID to) const
{
    return via_ways.count(std::make_pair(from, to)) > 0;
}

std::size_t WayRestrictionMap::Size() const { return via_ways.size(); }

std::size_t WayRestrictionMap::GetID(const NodeID from, const NodeID to) const
{
    return via_ways.find(std::make_pair(from, to))->second;
}

TurnRestriction const &WayRestrictionMap::GetRestriction(const std::size_t id) const
{
    return restriction_data[id];
}

std::vector<std::pair<NodeID, NodeID>> WayRestrictionMap::ViaWays() const
{
    std::vector<std::pair<NodeID, NodeID>> result;
    result.reserve(via_ways.size());
    std::transform(via_ways.begin(),
                   via_ways.end(),
                   std::back_inserter(result),
                   [](auto const pair) { return pair.first; });
    return result;
}

} // namespace extractor
} // namespace osrm
