#include "extractor/guidance/segregated_intersection_classification.hpp"
#include "extractor/guidance/coordinate_extractor.hpp"
#include "extractor/node_based_graph_factory.hpp"

#include "util/coordinate_calculation.hpp"
#include "util/name_table.hpp"

namespace osrm
{
namespace extractor
{
namespace guidance
{

void DumpBestNodes(std::vector<util::Coordinate> const & coordinates,
                   std::vector<std::pair<NodeID, size_t>> & nodes,
                   size_t count)
{
  if (nodes.empty())
    return;

  std::sort(nodes.begin(), nodes.end(), [](auto const & r1, auto const & r2)
  {
    return r1.second > r2.second;
  });

  struct PointT
  {
    double lon, lat;
    explicit PointT(util::Coordinate const & c)
      : lon(util::toFloating(c.lon)), lat(util::toFloating(c.lat))
    {
    }
  };

  struct ComparePoints
  {
    double epsLon, epsLat;

    ComparePoints(double epsMetres, double currentLat)
    {
      BOOST_ASSERT(fabs(currentLat) < 85.0);
      double const metresInLon = 40075000.0/360.0;
      epsLon = epsMetres / metresInLon;
      epsLat = epsMetres / (metresInLon * cos(currentLat));
    }

    bool operator() (PointT const & p1, PointT const & p2) const
    {
      if (p1.lon + epsLon < p2.lon)
        return true;
      if (p1.lon > p2.lon + epsLon)
        return false;

      // here p1.lon equals to p2.lon

      // true, if real less, false instead
      return (p1.lat + epsLat < p2.lat);
    }
  };

  double const lat(util::toFloating(coordinates[nodes.front().first].lat));
  std::set<PointT, ComparePoints> pointsSet(ComparePoints(50.0, lat));

  for (auto const & r : nodes)
  {
    if (count == 0)
      break;

    if (pointsSet.insert(PointT(coordinates[r.first])).second)
      --count;
  }

  std::ofstream of("points.txt");
  for (auto const & r : pointsSet)
  {
    // lon,lat
    of << r.lon << "," << r.lat << std::endl;
  }
}

struct EdgeInfo
{
    NodeID node;

    util::StringView name;

    // 0 - outgoing (forward), 1 - incoming (reverse), 2 - both outgoing and incoming
    int direction;

    ClassData road_class;

    RoadPriorityClass::Enum road_priority_class;

    struct LessName
    {
        bool operator()(EdgeInfo const &e1, EdgeInfo const &e2) const { return e1.name < e2.name; }
    };
};

bool IsSegregated(std::vector<EdgeInfo> v1,
                  std::vector<EdgeInfo> v2,
                  EdgeInfo const &current,
                  double edgeLength)
{
    if (v1.size() < 2 || v2.size() < 2)
        return false;

    auto const sort_by_name_fn = [](std::vector<EdgeInfo> &v) {
        std::sort(v.begin(), v.end(), EdgeInfo::LessName());
    };

    sort_by_name_fn(v1);
    sort_by_name_fn(v2);

    // Internal edge with the name should be connected with any other neibour edge with the same
    // name, e.g. isolated edge with unique name is not segregated.
    //              b - 'b' road continues here
    //              |
    //      - - a - |
    //              b - segregated edge
    //      - - a - |
    if (!current.name.empty())
    {
        auto const findNameFn = [&current](std::vector<EdgeInfo> const &v) {
            return std::binary_search(v.begin(), v.end(), current, EdgeInfo::LessName());
        };

        if (!findNameFn(v1) && !findNameFn(v2))
            return false;
    }

    // set_intersection like routine to get equal result pairs
    std::vector<std::pair<EdgeInfo const *, EdgeInfo const *>> commons;

    auto i1 = v1.begin();
    auto i2 = v2.begin();

    while (i1 != v1.end() && i2 != v2.end())
    {
        if (i1->name == i2->name)
        {
            if (!i1->name.empty())
                commons.push_back(std::make_pair(&(*i1), &(*i2)));

            ++i1;
            ++i2;
        }
        else if (i1->name < i2->name)
            ++i1;
        else
            ++i2;
    }

    if (commons.size() < 2)
        return false;

    auto const check_equal_class = [](std::pair<EdgeInfo const *, EdgeInfo const *> const &e) {
        // Or (e.first->road_class & e.second->road_class != 0)
        return e.first->road_class == e.second->road_class;
    };

    size_t equal_class_count = 0;
    for (auto const &e : commons)
        if (check_equal_class(e))
            ++equal_class_count;

    if (equal_class_count < 2)
        return false;

    auto const get_length_threshold = [](EdgeInfo const *e) {
        switch (e->road_priority_class)
        {
        case RoadPriorityClass::MOTORWAY:
        case RoadPriorityClass::TRUNK:
            return 30.0;
        case RoadPriorityClass::PRIMARY:
            return 20.0;
        case RoadPriorityClass::SECONDARY:
        case RoadPriorityClass::TERTIARY:
            return 10.0;
        default:
            return 5.0;
        }
    };

    double threshold = std::numeric_limits<double>::max();
    for (auto const &e : commons)
        threshold =
            std::min(threshold, get_length_threshold(e.first) + get_length_threshold(e.second));

    return edgeLength <= threshold;
}

std::unordered_set<EdgeID> findSegregatedNodes(const NodeBasedGraphFactory &factory,
                                               const util::NameTable &names)
{
    auto const &graph = factory.GetGraph();
    auto const &annotation = factory.GetAnnotationData();

    CoordinateExtractor coordExtractor(
        graph, factory.GetCompressedEdges(), factory.GetCoordinates());

    auto const get_edge_length = [&](NodeID from_node, EdgeID edgeID, NodeID to_node) {
        auto const geom = coordExtractor.GetCoordinatesAlongRoad(from_node, edgeID, false, to_node);
        double length = 0.0;
        for (size_t i = 1; i < geom.size(); ++i)
        {
            length += util::coordinate_calculation::haversineDistance(geom[i - 1], geom[i]);
        }
        return length;
    };

    auto const get_edge_info = [&](NodeID node, auto const &edgeData) -> EdgeInfo {
        /// @todo Make string normalization/lowercase/trim for comparison ...

        auto const id = annotation[edgeData.annotation_data].name_id;
        BOOST_ASSERT(id != INVALID_NAMEID);
        auto const name = names.GetNameForID(id);

        return {node,
                name,
                edgeData.reversed ? 1 : 0,
                annotation[edgeData.annotation_data].classes,
                edgeData.flags.road_classification.GetClass()};
    };

    auto const collect_edge_info_fn = [&](auto const &edges1, NodeID node2) {
        std::vector<EdgeInfo> info;

        for (auto const &e : edges1)
        {
            NodeID const target = graph.GetTarget(e);
            if (target == node2)
                continue;

            info.push_back(get_edge_info(target, graph.GetEdgeData(e)));
        }

        if (info.empty())
            return info;

        std::sort(info.begin(), info.end(), [](EdgeInfo const &e1, EdgeInfo const &e2) {
            return e1.node < e2.node;
        });

        // Merge equal infos with correct direction.
        auto curr = info.begin();
        auto next = curr;
        while (++next != info.end())
        {
            if (curr->node == next->node)
            {
                BOOST_ASSERT(curr->name == next->name);
                BOOST_ASSERT(curr->road_class == next->road_class);
                BOOST_ASSERT(curr->direction != next->direction);
                curr->direction = 2;
            }
            else
                curr = next;
        }

        info.erase(
            std::unique(info.begin(),
                        info.end(),
                        [](EdgeInfo const &e1, EdgeInfo const &e2) { return e1.node == e2.node; }),
            info.end());

        return info;
    };

    auto const isSegregatedFn = [&](auto const &edgeData,
                                    auto const &edges1,
                                    NodeID node1,
                                    auto const &edges2,
                                    NodeID node2,
                                    double edgeLength) {
        return IsSegregated(collect_edge_info_fn(edges1, node2),
                            collect_edge_info_fn(edges2, node1),
                            get_edge_info(node1, edgeData),
                            edgeLength);
    };

    std::unordered_set<EdgeID> segregated_edges;
    std::vector<std::pair<NodeID, size_t>> dump_nodes;

    for (NodeID sourceID = 0; sourceID < graph.GetNumberOfNodes(); ++sourceID)
    {
        auto const sourceEdges = graph.GetAdjacentEdgeRange(sourceID);
        for (EdgeID edgeID : sourceEdges)
        {
            auto const &edgeData = graph.GetEdgeData(edgeID);

            if (edgeData.reversed)
                continue;

            NodeID const targetID = graph.GetTarget(edgeID);
            auto const targetEdges = graph.GetAdjacentEdgeRange(targetID);

            double const length = get_edge_length(sourceID, edgeID, targetID);
            if (isSegregatedFn(edgeData, sourceEdges, sourceID, targetEdges, targetID, length))
            {
                segregated_edges.insert(edgeID);
                dump_nodes.push_back(std::make_pair(sourceID, sourceEdges.size() + targetEdges.size()));
            }
        }
    }

    DumpBestNodes(factory.GetCoordinates(), dump_nodes, 2000);

    return segregated_edges;
}

void FindTopInterestngCrossings(const NodeBasedGraphFactory &factory, size_t count)
{
  auto const & graph = factory.GetGraph();

  std::vector<std::pair<NodeID, size_t>> results;
  for (NodeID sourceID = 0; sourceID < graph.GetNumberOfNodes(); ++sourceID)
  {
    auto const edges = graph.GetAdjacentEdgeRange(sourceID);
    results.push_back(std::make_pair(sourceID, edges.size()));
  }

  DumpBestNodes(factory.GetCoordinates(), results, count);
}

}
}
}
