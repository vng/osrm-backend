#include "extractor/edge_based_graph_factory.hpp"
#include "extractor/conditional_turn_penalty.hpp"
#include "extractor/edge_based_edge.hpp"
#include "extractor/files.hpp"
#include "extractor/guidance/turn_analysis.hpp"
#include "extractor/guidance/turn_lane_handler.hpp"
#include "extractor/scripting_environment.hpp"
#include "extractor/suffix_table.hpp"

#include "extractor/intersection/intersection_analysis.hpp"

#include "extractor/serialization.hpp"
#include "storage/io.hpp"

#include "util/assert.hpp"
#include "util/bearing.hpp"
#include "util/coordinate.hpp"
#include "util/coordinate_calculation.hpp"
#include "util/exception.hpp"
#include "util/guidance/turn_bearing.hpp"
#include "util/integer_range.hpp"
#include "util/log.hpp"
#include "util/percent.hpp"
#include "util/timing_util.hpp"

#include <boost/assert.hpp>
#include <boost/functional/hash.hpp>
#include <boost/numeric/conversion/cast.hpp>

#include "boost/unordered_map.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_pipeline.h>

namespace std
{
template <> struct hash<std::pair<NodeID, NodeID>>
{
    std::size_t operator()(const std::pair<NodeID, NodeID> &mk) const noexcept
    {
        std::size_t seed = 0;
        boost::hash_combine(seed, mk.first);
        boost::hash_combine(seed, mk.second);
        return seed;
    }
};
} // namespace std

namespace osrm
{
namespace extractor
{

// Configuration to find representative candidate for turn angle calculations
EdgeBasedGraphFactory::EdgeBasedGraphFactory(
    const util::NodeBasedDynamicGraph &node_based_graph,
    const util::NodeBasedDynamicGraph &nbg_uncompressed,
    const extractor::PackedOSMIDs &osm_node_ids,
    EdgeBasedNodeDataContainer &node_data_container,
    const CompressedEdgeContainer &compressed_edge_container,
    const std::unordered_set<NodeID> &barrier_nodes,
    const std::unordered_set<NodeID> &traffic_lights,
    const std::vector<util::Coordinate> &coordinates,
    const util::NameTable &name_table,
    const std::unordered_set<EdgeID> &segregated_edges,
    guidance::LaneDescriptionMap &lane_description_map)
    : m_edge_based_node_container(node_data_container), m_osm_node_ids(osm_node_ids),
      m_number_of_edge_based_nodes(0), m_coordinates(coordinates),
      m_node_based_graph(node_based_graph), m_nbg_uncompressed(nbg_uncompressed),
      m_barrier_nodes(barrier_nodes), m_traffic_lights(traffic_lights),
      m_compressed_edge_container(compressed_edge_container), name_table(name_table),
      segregated_edges(segregated_edges), lane_description_map(lane_description_map)
{
}

void EdgeBasedGraphFactory::GetEdgeBasedEdges(
    util::DeallocatingVector<EdgeBasedEdge> &output_edge_list)
{
    BOOST_ASSERT_MSG(0 == output_edge_list.size(), "Vector is not empty");
    using std::swap; // Koenig swap
    swap(m_edge_based_edge_list, output_edge_list);
}

void EdgeBasedGraphFactory::GetEdgeBasedNodeSegments(std::vector<EdgeBasedNodeSegment> &nodes)
{
    using std::swap; // Koenig swap
    swap(nodes, m_edge_based_node_segments);
}

void EdgeBasedGraphFactory::GetStartPointMarkers(std::vector<bool> &node_is_startpoint)
{
    using std::swap; // Koenig swap
    swap(m_edge_based_node_is_startpoint, node_is_startpoint);
}

void EdgeBasedGraphFactory::GetEdgeBasedNodeWeights(std::vector<EdgeWeight> &output_node_weights)
{
    using std::swap; // Koenig swap
    swap(m_edge_based_node_weights, output_node_weights);
}

std::uint64_t EdgeBasedGraphFactory::GetNumberOfEdgeBasedNodes() const
{
    return m_number_of_edge_based_nodes;
}

NBGToEBG EdgeBasedGraphFactory::InsertEdgeBasedNode(const NodeID node_u, const NodeID node_v)
{
    // merge edges together into one EdgeBasedNode
    BOOST_ASSERT(node_u != SPECIAL_NODEID);
    BOOST_ASSERT(node_v != SPECIAL_NODEID);

    // find forward edge id and
    const EdgeID edge_id_1 = m_node_based_graph.FindEdge(node_u, node_v);
    BOOST_ASSERT(edge_id_1 != SPECIAL_EDGEID);

    const EdgeData &forward_data = m_node_based_graph.GetEdgeData(edge_id_1);

    // find reverse edge id and
    const EdgeID edge_id_2 = m_node_based_graph.FindEdge(node_v, node_u);
    BOOST_ASSERT(edge_id_2 != SPECIAL_EDGEID);

    const EdgeData &reverse_data = m_node_based_graph.GetEdgeData(edge_id_2);

    BOOST_ASSERT(nbe_to_ebn_mapping[edge_id_1] != SPECIAL_NODEID ||
                 nbe_to_ebn_mapping[edge_id_2] != SPECIAL_NODEID);

    if (nbe_to_ebn_mapping[edge_id_1] != SPECIAL_NODEID &&
        nbe_to_ebn_mapping[edge_id_2] == SPECIAL_NODEID)
        m_edge_based_node_weights[nbe_to_ebn_mapping[edge_id_1]] = INVALID_EDGE_WEIGHT;

    BOOST_ASSERT(m_compressed_edge_container.HasEntryForID(edge_id_1) ==
                 m_compressed_edge_container.HasEntryForID(edge_id_2));
    BOOST_ASSERT(m_compressed_edge_container.HasEntryForID(edge_id_1));
    BOOST_ASSERT(m_compressed_edge_container.HasEntryForID(edge_id_2));
    const auto &forward_geometry = m_compressed_edge_container.GetBucketReference(edge_id_1);
    BOOST_ASSERT(forward_geometry.size() ==
                 m_compressed_edge_container.GetBucketReference(edge_id_2).size());
    const auto segment_count = forward_geometry.size();

    // There should always be some geometry
    BOOST_ASSERT(0 != segment_count);

    // const unsigned packed_geometry_id = m_compressed_edge_container.ZipEdges(edge_id_1,
    // edge_id_2);

    NodeID current_edge_source_coordinate_id = node_u;

    const auto edge_id_to_segment_id = [](const NodeID edge_based_node_id) {
        if (edge_based_node_id == SPECIAL_NODEID)
        {
            return SegmentID{SPECIAL_SEGMENTID, false};
        }

        return SegmentID{edge_based_node_id, true};
    };

    // Add edge-based node data for forward and reverse nodes indexed by edge_id
    BOOST_ASSERT(nbe_to_ebn_mapping[edge_id_1] != SPECIAL_EDGEID);
    m_edge_based_node_container.nodes[nbe_to_ebn_mapping[edge_id_1]].geometry_id =
        forward_data.geometry_id;
    m_edge_based_node_container.nodes[nbe_to_ebn_mapping[edge_id_1]].annotation_id =
        forward_data.annotation_data;
    m_edge_based_node_container.nodes[nbe_to_ebn_mapping[edge_id_1]].segregated =
        segregated_edges.count(edge_id_1) > 0;

    if (nbe_to_ebn_mapping[edge_id_2] != SPECIAL_EDGEID)
    {
        m_edge_based_node_container.nodes[nbe_to_ebn_mapping[edge_id_2]].geometry_id =
            reverse_data.geometry_id;
        m_edge_based_node_container.nodes[nbe_to_ebn_mapping[edge_id_2]].annotation_id =
            reverse_data.annotation_data;
        m_edge_based_node_container.nodes[nbe_to_ebn_mapping[edge_id_2]].segregated =
            segregated_edges.count(edge_id_2) > 0;
    }

    // Add segments of edge-based nodes
    for (const auto i : util::irange(std::size_t{0}, segment_count))
    {
        BOOST_ASSERT(
            current_edge_source_coordinate_id ==
            m_compressed_edge_container.GetBucketReference(edge_id_2)[segment_count - 1 - i]
                .node_id);
        const NodeID current_edge_target_coordinate_id = forward_geometry[i].node_id;

        // don't add node-segments for penalties
        if (current_edge_target_coordinate_id == current_edge_source_coordinate_id)
            continue;

        BOOST_ASSERT(current_edge_target_coordinate_id != current_edge_source_coordinate_id);

        // build edges
        m_edge_based_node_segments.emplace_back(
            edge_id_to_segment_id(nbe_to_ebn_mapping[edge_id_1]),
            edge_id_to_segment_id(nbe_to_ebn_mapping[edge_id_2]),
            current_edge_source_coordinate_id,
            current_edge_target_coordinate_id,
            i);

        m_edge_based_node_is_startpoint.push_back(forward_data.flags.startpoint ||
                                                  reverse_data.flags.startpoint);
        current_edge_source_coordinate_id = current_edge_target_coordinate_id;
    }

    BOOST_ASSERT(current_edge_source_coordinate_id == node_v);

    return NBGToEBG{node_u, node_v, nbe_to_ebn_mapping[edge_id_1], nbe_to_ebn_mapping[edge_id_2]};
}

void EdgeBasedGraphFactory::Run(ScriptingEnvironment &scripting_environment,
                                const std::string &turn_data_filename,
                                const std::string &turn_lane_data_filename,
                                const std::string &turn_weight_penalties_filename,
                                const std::string &turn_duration_penalties_filename,
                                const std::string &turn_penalties_index_filename,
                                const std::string &cnbg_ebg_mapping_path,
                                const std::string &conditional_penalties_filename,
                                const std::string &geometry_info_filename,
                                const RestrictionMap &node_restriction_map,
                                const ConditionalRestrictionMap &conditional_node_restriction_map,
                                const WayRestrictionMap &way_restriction_map)
{
    TIMER_START(renumber);
    m_number_of_edge_based_nodes =
        LabelEdgeBasedNodes() + way_restriction_map.NumberOfDuplicatedNodes();
    TIMER_STOP(renumber);

    // Allocate memory for edge-based nodes
    // In addition to the normal edges, allocate enough space for copied edges from
    // via-way-restrictions, see calculation above
    m_edge_based_node_container.nodes.resize(m_number_of_edge_based_nodes);

    TIMER_START(generate_nodes);
    {
        auto mapping = GenerateEdgeExpandedNodes(way_restriction_map);
        files::writeNBGMapping(cnbg_ebg_mapping_path, mapping);
    }
    TIMER_STOP(generate_nodes);

    TIMER_START(generate_edges);
    GenerateEdgeExpandedEdges(scripting_environment,
                              turn_data_filename,
                              turn_lane_data_filename,
                              turn_weight_penalties_filename,
                              turn_duration_penalties_filename,
                              turn_penalties_index_filename,
                              conditional_penalties_filename,
                              node_restriction_map,
                              conditional_node_restriction_map,
                              way_restriction_map);

    TIMER_STOP(generate_edges);

    util::Log() << "Save extracted geometry info ...";
    m_ebn_geometry_info.Save(geometry_info_filename);

    util::Log() << "Timing statistics for edge-expanded graph:";
    util::Log() << "Renumbering edges: " << TIMER_SEC(renumber) << "s";
    util::Log() << "Generating nodes: " << TIMER_SEC(generate_nodes) << "s";
    util::Log() << "Generating edges: " << TIMER_SEC(generate_edges) << "s";
}

/// Renumbers all _forward_ edges and sets the edge_id.
/// A specific numbering is not important. Any unique ID will do.
/// Returns the number of edge-based nodes.
unsigned EdgeBasedGraphFactory::LabelEdgeBasedNodes()
{
    // heuristic: node-based graph node is a simple intersection with four edges (edge-based nodes)
    m_edge_based_node_weights.reserve(4 * m_node_based_graph.GetNumberOfNodes());
    m_ebn_geometry_info.reserve(4 * m_node_based_graph.GetNumberOfNodes());
    nbe_to_ebn_mapping.resize(m_node_based_graph.GetEdgeCapacity(), SPECIAL_NODEID);

    // renumber edge based node of outgoing edges
    unsigned numbered_edges_count = 0;
    for (const auto current_node : util::irange(0u, m_node_based_graph.GetNumberOfNodes()))
    {
        for (const auto current_edge : m_node_based_graph.GetAdjacentEdgeRange(current_node))
        {
            const EdgeData &edge_data = m_node_based_graph.GetEdgeData(current_edge);
            // only number incoming edges
            if (edge_data.reversed)
            {
                continue;
            }

            m_edge_based_node_weights.push_back(edge_data.weight);

            m_ebn_geometry_info.Add(GetGeometryInfo(current_node, current_edge, edge_data));

            BOOST_ASSERT(numbered_edges_count < m_node_based_graph.GetNumberOfEdges());
            nbe_to_ebn_mapping[current_edge] = numbered_edges_count++;
        }
    }

    return numbered_edges_count;
}

/// Creates the nodes in the edge expanded graph from edges in the node-based graph.
std::vector<NBGToEBG>
EdgeBasedGraphFactory::GenerateEdgeExpandedNodes(const WayRestrictionMap &way_restriction_map)
{
    std::vector<NBGToEBG> mapping;

    util::Log() << "Generating edge expanded nodes ... ";
    // indicating a normal node within the edge-based graph. This node represents an edge in the
    // node-based graph
    {
        util::UnbufferedLog log;
        util::Percent progress(log, m_node_based_graph.GetNumberOfNodes());

        // m_compressed_edge_container.InitializeBothwayVector();

        // loop over all edges and generate new set of nodes
        for (const auto nbg_node_u : util::irange(0u, m_node_based_graph.GetNumberOfNodes()))
        {
            BOOST_ASSERT(nbg_node_u != SPECIAL_NODEID);
            progress.PrintStatus(nbg_node_u);
            for (EdgeID nbg_edge_id : m_node_based_graph.GetAdjacentEdgeRange(nbg_node_u))
            {
                BOOST_ASSERT(nbg_edge_id != SPECIAL_EDGEID);

                const NodeID nbg_node_v = m_node_based_graph.GetTarget(nbg_edge_id);
                BOOST_ASSERT(nbg_node_v != SPECIAL_NODEID);
                BOOST_ASSERT(nbg_node_u != nbg_node_v);

                // pick only every other edge, since we have every edge as an outgoing
                // and incoming egde
                if (nbg_node_u >= nbg_node_v)
                {
                    continue;
                }

                // if we found a non-forward edge reverse and try again
                if (nbe_to_ebn_mapping[nbg_edge_id] == SPECIAL_NODEID)
                {
                    mapping.push_back(InsertEdgeBasedNode(nbg_node_v, nbg_node_u));
                }
                else
                {
                    mapping.push_back(InsertEdgeBasedNode(nbg_node_u, nbg_node_v));
                }
            }
        }
    }

    util::Log() << "Expanding via-way turn restrictions ... ";
    // Add copies of the nodes
    {
        util::UnbufferedLog log;
        const auto via_ways = way_restriction_map.DuplicatedNodeRepresentatives();
        util::Percent progress(log, via_ways.size());

        NodeID edge_based_node_id =
            NodeID(m_number_of_edge_based_nodes - way_restriction_map.NumberOfDuplicatedNodes());
        std::size_t progress_counter = 0;
        // allocate enough space for the mapping
        for (const auto way : via_ways)
        {
            const auto node_u = way.from;
            const auto node_v = way.to;
            // we know that the edge exists as non-reversed edge
            const auto eid = m_node_based_graph.FindEdge(node_u, node_v);

            BOOST_ASSERT(nbe_to_ebn_mapping[eid] != SPECIAL_NODEID);

            // merge edges together into one EdgeBasedNode
            BOOST_ASSERT(node_u != SPECIAL_NODEID);
            BOOST_ASSERT(node_v != SPECIAL_NODEID);

            // find node in the edge based graph, we only require one id:
            const EdgeData &edge_data = m_node_based_graph.GetEdgeData(eid);
            // BOOST_ASSERT(edge_data.edge_id < m_edge_based_node_container.Size());
            m_edge_based_node_container.nodes[edge_based_node_id].geometry_id =
                edge_data.geometry_id;
            m_edge_based_node_container.nodes[edge_based_node_id].annotation_id =
                edge_data.annotation_data;
            m_edge_based_node_container.nodes[edge_based_node_id].segregated =
                segregated_edges.count(eid) > 0;

            const auto ebn_weight = m_edge_based_node_weights[nbe_to_ebn_mapping[eid]];
            BOOST_ASSERT(ebn_weight == INVALID_EDGE_WEIGHT || ebn_weight == edge_data.weight);
            m_edge_based_node_weights.push_back(ebn_weight);

            // Same as GetGeometryInfo(node_u, eid, edge_data), but should be faster ..
            m_ebn_geometry_info.Add(m_ebn_geometry_info[nbe_to_ebn_mapping[eid]]);

            edge_based_node_id++;
            progress.PrintStatus(progress_counter++);
        }
    }

    BOOST_ASSERT(m_edge_based_node_segments.size() == m_edge_based_node_is_startpoint.size());
    BOOST_ASSERT(m_number_of_edge_based_nodes == m_edge_based_node_weights.size());

    util::Log() << "Generated " << m_number_of_edge_based_nodes << " nodes ("
                << way_restriction_map.NumberOfDuplicatedNodes()
                << " of which are duplicates)  and " << m_edge_based_node_segments.size()
                << " segments in edge-expanded graph";

    return mapping;
}

/// Actually it also generates turn data and serializes them...
void EdgeBasedGraphFactory::GenerateEdgeExpandedEdges(
    ScriptingEnvironment &scripting_environment,
    const std::string &turn_data_filename,
    const std::string &turn_lane_data_filename,
    const std::string &turn_weight_penalties_filename,
    const std::string &turn_duration_penalties_filename,
    const std::string &turn_penalties_index_filename,
    const std::string &conditional_penalties_filename,
    const RestrictionMap &node_restriction_map,
    const ConditionalRestrictionMap &conditional_restriction_map,
    const WayRestrictionMap &way_restriction_map)
{

    util::Log() << "Generating edge-expanded edges ";

    std::size_t node_based_edge_counter = 0;
    restricted_turns_counter = 0;
    skipped_uturns_counter = 0;
    skipped_barrier_turns_counter = 0;

    storage::io::FileWriter turn_penalties_index_file(turn_penalties_index_filename,
                                                      storage::io::FileWriter::HasNoFingerprint);

    TurnDataExternalContainer turn_data_container;

    SuffixTable street_name_suffix_table(scripting_environment);
    const auto &turn_lanes_data = transformTurnLaneMapIntoArrays(lane_description_map);
    guidance::MergableRoadDetector mergable_road_detector(m_node_based_graph,
                                                          m_edge_based_node_container,
                                                          m_coordinates,
                                                          m_compressed_edge_container,
                                                          node_restriction_map,
                                                          m_barrier_nodes,
                                                          turn_lanes_data,
                                                          name_table,
                                                          street_name_suffix_table);

    // Loop over all turns and generate new set of edges.
    // Three nested loop look super-linear, but we are dealing with a (kind of)
    // linear number of turns only.
    guidance::TurnAnalysis turn_analysis(m_node_based_graph,
                                         m_edge_based_node_container,
                                         m_coordinates,
                                         m_compressed_edge_container,
                                         node_restriction_map,
                                         m_barrier_nodes,
                                         turn_lanes_data,
                                         name_table,
                                         street_name_suffix_table);

    util::guidance::LaneDataIdMap lane_data_map;
    guidance::lanes::TurnLaneHandler turn_lane_handler(m_node_based_graph,
                                                       m_edge_based_node_container,
                                                       m_coordinates,
                                                       m_compressed_edge_container,
                                                       node_restriction_map,
                                                       m_barrier_nodes,
                                                       turn_lanes_data,
                                                       lane_description_map,
                                                       turn_analysis,
                                                       lane_data_map);

    bearing_class_by_node_based_node.resize(m_node_based_graph.GetNumberOfNodes(),
                                            std::numeric_limits<std::uint32_t>::max());

    // FIXME these need to be tuned in pre-allocated size
    std::vector<TurnPenalty> turn_weight_penalties;
    std::vector<TurnPenalty> turn_duration_penalties;

    const auto weight_multiplier =
        scripting_environment.GetProfileProperties().GetWeightMultiplier();

    // filled in during next stage, kept alive through following scope
    std::vector<Conditional> conditionals;
    // The following block generates the edge-based-edges using a parallel processing
    // pipeline.  Sets of intersection IDs are batched in groups of GRAINSIZE (100)
    // `generator_stage`,
    // then those groups are processed in parallel `processor_stage`.  Finally, results are
    // appended to the various buffer vectors by the `output_stage` in the same order
    // that the `generator_stage` created them in (tbb::filter_mode::serial_in_order creates this
    // guarantee).  The order needs to be maintained because we depend on it later in the
    // processing pipeline.
    {
        util::UnbufferedLog log;

        const NodeID node_count = m_node_based_graph.GetNumberOfNodes();
        util::Percent progress(log, node_count);
        // This counter is used to keep track of how far along we've made it
        std::uint64_t nodes_completed = 0;

        // going over all nodes (which form the center of an intersection), we compute all
        // possible turns along these intersections.

        NodeID current_node = 0;

        // Handle intersections in sets of 100.  The pipeline below has a serial bottleneck
        // during the writing phase, so we want to make the parallel workers do more work
        // to give the serial final stage time to complete its tasks.
        const constexpr unsigned GRAINSIZE = 100;

        // First part of the pipeline generates iterator ranges of IDs in sets of GRAINSIZE
        tbb::filter<void, tbb::blocked_range<NodeID>> generator_stage(
            tbb::filter_mode::serial_in_order, [&](tbb::flow_control &fc) -> tbb::blocked_range<NodeID> {
                if (current_node < node_count)
                {
                    auto next_node = std::min(current_node + GRAINSIZE, node_count);
                    auto result = tbb::blocked_range<NodeID>(current_node, next_node);
                    current_node = next_node;
                    return result;
                }
                else
                {
                    fc.stop();
                    return tbb::blocked_range<NodeID>(node_count, node_count);
                }
            });

        // This struct is the buffered output of the `processor_stage`.  This data is
        // appended to the various output arrays/files by the `output_stage`.
        struct IntersectionData
        {
            std::vector<lookup::TurnIndexBlock> turn_indexes;
            std::vector<EdgeBasedEdge> edges_list;
            std::vector<TurnPenalty> turn_weight_penalties;
            std::vector<TurnPenalty> turn_duration_penalties;
            std::vector<TurnData> turn_data_container;
        };

        // same as IntersectionData, but grouped with edge to allow sorting after creating. Edges
        // can be out of order
        struct EdgeWithData
        {
            EdgeBasedEdge edge;
            lookup::TurnIndexBlock turn_index;
            TurnPenalty turn_weight_penalty;
            TurnPenalty turn_duration_penalty;
            TurnData turn_data;
        };

        struct PipelineBuffer
        {
            std::size_t nodes_processed = 0;
            IntersectionData continuous_data;
            std::vector<EdgeWithData> delayed_data;
            std::vector<Conditional> conditionals;
        };

        // Generate edges for either artificial nodes or the main graph
        const auto generate_edge = [this,
                                    &scripting_environment,
                                    weight_multiplier,
                                    &conditional_restriction_map](
            // what nodes will be used? In most cases this will be the id stored in the edge_data.
            // In case of duplicated nodes (e.g. due to via-way restrictions), one/both of these
            // might refer to a newly added edge based node
            const auto edge_based_node_from,
            const auto edge_based_node_to,
            // the situation of the turn
            const auto node_along_road_entering,
            const auto node_based_edge_from,
            const auto intersection_node,
            const auto node_based_edge_to,
            const auto incoming_bearing,
            const auto &turn,
            const auto entry_class_id) {

            const auto node_restricted = isRestricted(node_along_road_entering,
                                                      intersection_node,
                                                      m_node_based_graph.GetTarget(turn.eid),
                                                      conditional_restriction_map);

            boost::optional<Conditional> conditional = boost::none;
            if (node_restricted.first)
            {
                auto const &conditions = node_restricted.second->condition;
                // get conditions of the restriction limiting the node
                conditional = {{edge_based_node_from,
                                edge_based_node_to,
                                {static_cast<std::uint64_t>(-1),
                                 m_coordinates[intersection_node],
                                 conditions}}};
            }

            const auto &edge_data1 = m_node_based_graph.GetEdgeData(node_based_edge_from);
            const auto &edge_data2 = m_node_based_graph.GetEdgeData(node_based_edge_to);

            BOOST_ASSERT(nbe_to_ebn_mapping[node_based_edge_from] !=
                         nbe_to_ebn_mapping[node_based_edge_to]);
            BOOST_ASSERT(!edge_data1.reversed);
            BOOST_ASSERT(!edge_data2.reversed);

            // the following is the core of the loop.
            TurnData turn_data = {turn.instruction,
                                  turn.lane_data_id,
                                  entry_class_id,
                                  util::guidance::TurnBearing(incoming_bearing),
                                  util::guidance::TurnBearing(turn.bearing)};

            // compute weight and duration penalties
            auto is_traffic_light = m_traffic_lights.count(intersection_node);
            ExtractionTurn extracted_turn(
                turn.angle,
                m_node_based_graph.GetOutDegree(intersection_node),
                turn.instruction.IsUTurn(),
                is_traffic_light,
                edge_data1.flags.restricted,
                edge_data2.flags.restricted,
                m_edge_based_node_container.GetAnnotation(edge_data1.annotation_data)
                    .is_left_hand_driving,
                m_edge_based_node_container.GetAnnotation(edge_data1.annotation_data).travel_mode,
                m_edge_based_node_container.GetAnnotation(edge_data2.annotation_data).travel_mode);
            scripting_environment.ProcessTurn(extracted_turn);

            // turn penalties are limited to [-2^15, 2^15) which roughly
            // translates to 54 minutes and fits signed 16bit deci-seconds
            auto weight_penalty =
                boost::numeric_cast<TurnPenalty>(extracted_turn.weight * weight_multiplier);
            auto duration_penalty = boost::numeric_cast<TurnPenalty>(extracted_turn.duration * 10.);

            BOOST_ASSERT(SPECIAL_NODEID != nbe_to_ebn_mapping[node_based_edge_from]);
            BOOST_ASSERT(SPECIAL_NODEID != nbe_to_ebn_mapping[node_based_edge_to]);

            // auto turn_id = m_edge_based_edge_list.size();
            auto weight = boost::numeric_cast<EdgeWeight>(edge_data1.weight + weight_penalty);
            auto duration = boost::numeric_cast<EdgeWeight>(edge_data1.duration + duration_penalty);

            EdgeBasedEdge edge_based_edge = {
                edge_based_node_from,
                edge_based_node_to,
                SPECIAL_NODEID, // This will be updated once the main loop
                                // completes!
                weight,
                duration,
                true,
                false};

            // We write out the mapping between the edge-expanded edges and
            // the original nodes. Since each edge represents a possible
            // maneuver, external programs can use this to quickly perform updates to edge
            // weights in order to penalize certain turns.

            // If this edge is 'trivial' -- where the compressed edge
            // corresponds exactly to an original OSM segment -- we can pull the turn's
            // preceding node ID directly with `node_along_road_entering`;
            // otherwise, we need to look up the node immediately preceding the turn
            // from the compressed edge container.
            const bool isTrivial = m_compressed_edge_container.IsTrivial(node_based_edge_from);

            const auto &from_node =
                isTrivial ? node_along_road_entering
                          : m_compressed_edge_container.GetLastEdgeSourceID(node_based_edge_from);
            const auto &to_node = m_compressed_edge_container.GetFirstEdgeTargetID(turn.eid);

            lookup::TurnIndexBlock turn_index_block = {from_node, intersection_node, to_node};

            // insert data into the designated buffer
            return std::make_pair(
                EdgeWithData{
                    edge_based_edge, turn_index_block, weight_penalty, duration_penalty, turn_data},
                conditional);
        };

        // Second part of the pipeline is where the intersection analysis is done for
        // each intersection
        tbb::filter<tbb::blocked_range<NodeID>, std::shared_ptr<PipelineBuffer>> processor_stage(
            tbb::filter_mode::parallel, [&](const tbb::blocked_range<NodeID> &intersection_node_range) {

                auto buffer = std::make_shared<PipelineBuffer>();
                buffer->nodes_processed =
                    intersection_node_range.end() - intersection_node_range.begin();

                // If we get fed a 0-length range for some reason, we can just return right away
                if (buffer->nodes_processed == 0)
                    return buffer;

                for (auto intersection_node = intersection_node_range.begin(),
                          end = intersection_node_range.end();
                     intersection_node < end;
                     ++intersection_node)
                {
                    // We capture the thread-local work in these objects, then flush
                    // them in a controlled manner at the end of the parallel range
                    const auto &incoming_edges =
                        intersection::getIncomingEdges(m_node_based_graph, intersection_node);
                    const auto &outgoing_edges =
                        intersection::getOutgoingEdges(m_node_based_graph, intersection_node);

                    intersection::IntersectionEdgeGeometries edge_geometries;
                    std::unordered_set<EdgeID> merged_edge_ids;
                    std::tie(edge_geometries, merged_edge_ids) =
                        intersection::getIntersectionGeometries(m_node_based_graph,
                                                                m_compressed_edge_container,
                                                                m_coordinates,
                                                                mergable_road_detector,
                                                                intersection_node);

                    // all nodes in the graph are connected in both directions. We check all
                    // outgoing nodes to find the incoming edge. This is a larger search overhead,
                    // but the cost we need to pay to generate edges here is worth the additional
                    // search overhead.
                    //
                    // a -> b <-> c
                    //      |
                    //      v
                    //      d
                    //
                    // will have:
                    // a: b,rev=0
                    // b: a,rev=1 c,rev=0 d,rev=0
                    // c: b,rev=0
                    //
                    // From the flags alone, we cannot determine which nodes are connected to
                    // `b` by an outgoing edge. Therefore, we have to search all connected edges for
                    // edges entering `b`

                    for (const auto &incoming_edge : incoming_edges)
                    {
                        ++node_based_edge_counter;

                        const auto intersection_view =
                            convertToIntersectionView(m_node_based_graph,
                                                      m_edge_based_node_container,
                                                      node_restriction_map,
                                                      m_barrier_nodes,
                                                      edge_geometries,
                                                      turn_lanes_data,
                                                      incoming_edge,
                                                      outgoing_edges,
                                                      merged_edge_ids);

                        auto intersection = turn_analysis.AssignTurnTypes(
                            incoming_edge.node, incoming_edge.edge, intersection_view);

                        OSRM_ASSERT(intersection.valid(), m_coordinates[intersection_node]);
                        intersection = turn_lane_handler.assignTurnLanes(
                            incoming_edge.node, incoming_edge.edge, std::move(intersection));

                        // the entry class depends on the turn, so we have to classify the
                        // interesction for every edge
                        const auto turn_classification =
                            classifyIntersection(intersection, m_coordinates[intersection_node]);

                        const auto entry_class_id =
                            entry_class_hash.ConcurrentFindOrAdd(turn_classification.first);

                        const auto bearing_class_id =
                            bearing_class_hash.ConcurrentFindOrAdd(turn_classification.second);

                        // Note - this is strictly speaking not thread safe, but we know we
                        // should never be touching the same element twice, so we should
                        // be fine.
                        bearing_class_by_node_based_node[intersection_node] = bearing_class_id;

                        // check if we are turning off a via way
                        const auto turning_off_via_way =
                            way_restriction_map.IsViaWay(incoming_edge.node, intersection_node);

                        // Save reversed incoming bearing to compute turn angles
                        const auto reversed_incoming_bearing = util::bearing::reverse(
                            findEdgeBearing(edge_geometries, incoming_edge.edge));

                        for (const auto &outgoing_edge : outgoing_edges)
                        {
                            if (!intersection::isTurnAllowed(m_node_based_graph,
                                                             m_edge_based_node_container,
                                                             node_restriction_map,
                                                             m_barrier_nodes,
                                                             edge_geometries,
                                                             turn_lanes_data,
                                                             incoming_edge,
                                                             outgoing_edge))
                                continue;

                            const auto turn =
                                std::find_if(intersection.begin(),
                                             intersection.end(),
                                             [edge = outgoing_edge.edge](const auto &road) {
                                                 return road.eid == edge;
                                             });
                            OSRM_ASSERT(turn != intersection.end(),
                                        m_coordinates[intersection_node]);

                            // In case a way restriction starts at a given location, add a turn onto
                            // every artificial node eminating here.
                            //
                            //     e - f
                            //     |
                            // a - b
                            //     |
                            //     c - d
                            //
                            // ab via bc to cd
                            // ab via be to ef
                            //
                            // has two artifical nodes (be/bc) with restrictions starting at `ab`.
                            // Since every restriction group (abc | abe) refers to the same
                            // artificial node, we simply have to find a single representative for
                            // the turn. Here we check whether the turn in question is the start of
                            // a via way restriction. If that should be the case, we switch
                            // the id of the edge-based-node for the target to the ID of the
                            // duplicated node associated with the turn. (e.g. ab via bc switches bc
                            // to bc_dup)
                            auto const target_id = way_restriction_map.RemapIfRestricted(
                                nbe_to_ebn_mapping[outgoing_edge.edge],
                                incoming_edge.node,
                                outgoing_edge.node,
                                m_node_based_graph.GetTarget(outgoing_edge.edge),
                                m_number_of_edge_based_nodes);

                            { // scope to forget edge_with_data after
                                const auto edge_with_data_and_condition =
                                    generate_edge(nbe_to_ebn_mapping[incoming_edge.edge],
                                                  target_id,
                                                  incoming_edge.node,
                                                  incoming_edge.edge,
                                                  outgoing_edge.node,
                                                  outgoing_edge.edge,
                                                  reversed_incoming_bearing,
                                                  *turn,
                                                  entry_class_id);

                                buffer->continuous_data.edges_list.push_back(
                                    edge_with_data_and_condition.first.edge);
                                buffer->continuous_data.turn_indexes.push_back(
                                    edge_with_data_and_condition.first.turn_index);
                                buffer->continuous_data.turn_weight_penalties.push_back(
                                    edge_with_data_and_condition.first.turn_weight_penalty);
                                buffer->continuous_data.turn_duration_penalties.push_back(
                                    edge_with_data_and_condition.first.turn_duration_penalty);
                                buffer->continuous_data.turn_data_container.push_back(
                                    edge_with_data_and_condition.first.turn_data);
                                if (edge_with_data_and_condition.second)
                                {
                                    buffer->conditionals.push_back(
                                        *edge_with_data_and_condition.second);
                                }
                            }

                            // when turning off a a via-way turn restriction, we need to not only
                            // handle the normal edges for the way, but also add turns for every
                            // duplicated node. This process is integrated here to avoid doing the
                            // turn analysis multiple times.
                            if (turning_off_via_way)
                            {
                                const auto duplicated_nodes = way_restriction_map.DuplicatedNodeIDs(
                                    incoming_edge.node, intersection_node);

                                // next to the normal restrictions tracked in `entry_allowed`, via
                                // ways might introduce additional restrictions. These are handled
                                // here when turning off a via-way
                                for (auto duplicated_node_id : duplicated_nodes)
                                {
                                    const auto from_id =
                                        NodeID(m_number_of_edge_based_nodes -
                                               way_restriction_map.NumberOfDuplicatedNodes() +
                                               duplicated_node_id);

                                    auto const node_at_end_of_turn =
                                        m_node_based_graph.GetTarget(outgoing_edge.edge);

                                    const auto is_way_restricted = way_restriction_map.IsRestricted(
                                        duplicated_node_id, node_at_end_of_turn);

                                    if (is_way_restricted)
                                    {
                                        auto const restriction = way_restriction_map.GetRestriction(
                                            duplicated_node_id, node_at_end_of_turn);

                                        if (restriction.condition.empty())
                                            continue;

                                        // add into delayed data
                                        auto edge_with_data_and_condition =
                                            generate_edge(from_id,
                                                          nbe_to_ebn_mapping[outgoing_edge.edge],
                                                          incoming_edge.node,
                                                          incoming_edge.edge,
                                                          outgoing_edge.node,
                                                          outgoing_edge.edge,
                                                          reversed_incoming_bearing,
                                                          *turn,
                                                          entry_class_id);

                                        buffer->delayed_data.push_back(
                                            std::move(edge_with_data_and_condition.first));

                                        if (edge_with_data_and_condition.second)
                                        {
                                            buffer->conditionals.push_back(
                                                *edge_with_data_and_condition.second);
                                        }

                                        // also add the conditions for the way
                                        if (is_way_restricted && !restriction.condition.empty())
                                        {
                                            // add a new conditional for the edge we just created
                                            buffer->conditionals.push_back(
                                                {from_id,
                                                 nbe_to_ebn_mapping[outgoing_edge.edge],
                                                 {static_cast<std::uint64_t>(-1),
                                                  m_coordinates[intersection_node],
                                                  restriction.condition}});
                                        }
                                    }
                                    else
                                    {
                                        auto edge_with_data_and_condition =
                                            generate_edge(from_id,
                                                          nbe_to_ebn_mapping[outgoing_edge.edge],
                                                          incoming_edge.node,
                                                          incoming_edge.edge,
                                                          outgoing_edge.node,
                                                          outgoing_edge.edge,
                                                          reversed_incoming_bearing,
                                                          *turn,
                                                          entry_class_id);

                                        buffer->delayed_data.push_back(
                                            std::move(edge_with_data_and_condition.first));

                                        if (edge_with_data_and_condition.second)
                                        {
                                            buffer->conditionals.push_back(
                                                *edge_with_data_and_condition.second);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                return buffer;
            });

        // Because we write TurnIndexBlock data as we go, we'll
        // buffer them into groups of 1000 to reduce the syscall
        // count by 1000x.  This doesn't need much memory, but
        // greatly reduces the syscall overhead of writing lots
        // of small objects
        const constexpr int TURN_INDEX_WRITE_BUFFER_SIZE = 1000;
        std::vector<lookup::TurnIndexBlock> turn_indexes_write_buffer;
        turn_indexes_write_buffer.reserve(TURN_INDEX_WRITE_BUFFER_SIZE);

        std::vector<EdgeWithData> delayed_data;

        // Last part of the pipeline puts all the calculated data into the serial buffers
        tbb::filter<std::shared_ptr<PipelineBuffer>, void> output_stage(
            tbb::filter_mode::serial_in_order, [&](const std::shared_ptr<PipelineBuffer> buffer) {
                nodes_completed += buffer->nodes_processed;
                progress.PrintStatus(nodes_completed);

                // for readability
                const auto &data = buffer->continuous_data;
                // NOTE: potential overflow here if we hit 2^32 routable edges
                m_edge_based_edge_list.append(data.edges_list.begin(), data.edges_list.end());

                BOOST_ASSERT(m_edge_based_edge_list.size() <= std::numeric_limits<NodeID>::max());

                turn_weight_penalties.insert(turn_weight_penalties.end(),
                                             data.turn_weight_penalties.begin(),
                                             data.turn_weight_penalties.end());
                turn_duration_penalties.insert(turn_duration_penalties.end(),
                                               data.turn_duration_penalties.begin(),
                                               data.turn_duration_penalties.end());
                turn_data_container.append(data.turn_data_container);
                turn_indexes_write_buffer.insert(turn_indexes_write_buffer.end(),
                                                 data.turn_indexes.begin(),
                                                 data.turn_indexes.end());

                conditionals.insert(
                    conditionals.end(), buffer->conditionals.begin(), buffer->conditionals.end());

                // Buffer writes to reduce syscall count
                if (turn_indexes_write_buffer.size() >= TURN_INDEX_WRITE_BUFFER_SIZE)
                {
                    turn_penalties_index_file.WriteFrom(turn_indexes_write_buffer.data(),
                                                        turn_indexes_write_buffer.size());
                    turn_indexes_write_buffer.clear();
                }

                delayed_data.insert(
                    delayed_data.end(), buffer->delayed_data.begin(), buffer->delayed_data.end());
            });

        // Now, execute the pipeline.  The value of "5" here was chosen by experimentation
        // on a 16-CPU machine and seemed to give the best performance.  This value needs
        // to be balanced with the GRAINSIZE above - ideally, the pipeline puts as much work
        // as possible in the `intersection_handler` step so that those parallel workers don't
        // get blocked too much by the slower (io-performing) `buffer_storage`
        tbb::parallel_pipeline(std::thread::hardware_concurrency() * 5,
                               generator_stage & processor_stage & output_stage);

        std::sort(delayed_data.begin(), delayed_data.end(), [](auto const &lhs, auto const &rhs) {
            return lhs.edge.source < rhs.edge.source;
        });
        auto const transfer_data = [&](auto const &edge_with_data) {
            m_edge_based_edge_list.push_back(edge_with_data.edge);
            turn_weight_penalties.push_back(edge_with_data.turn_weight_penalty);
            turn_duration_penalties.push_back(edge_with_data.turn_duration_penalty);
            turn_data_container.push_back(edge_with_data.turn_data);
            turn_indexes_write_buffer.push_back(edge_with_data.turn_index);
        };
        std::for_each(delayed_data.begin(), delayed_data.end(), transfer_data);

        // Flush the turn_indexes_write_buffer if it's not empty
        if (!turn_indexes_write_buffer.empty())
        {
            turn_penalties_index_file.WriteFrom(turn_indexes_write_buffer.data(),
                                                turn_indexes_write_buffer.size());
            turn_indexes_write_buffer.clear();
        }
    }

    util::Log() << "Reunmbering turns";
    // Now, update the turn_id property on every EdgeBasedEdge - it will equal the
    // position in the m_edge_based_edge_list array for each object.
    tbb::parallel_for(tbb::blocked_range<NodeID>(0, m_edge_based_edge_list.size()),
                      [this](const tbb::blocked_range<NodeID> &range) {
                          for (auto x = range.begin(), end = range.end(); x != end; ++x)
                          {
                              m_edge_based_edge_list[x].data.turn_id = x;
                          }
                      });

    // re-hash conditionals to ocnnect to their respective edge-based edges. Due to the
    // ordering, we
    // do not really have a choice but to index the conditional penalties and walk over all
    // edge-based-edges to find the ID of the edge
    auto const indexed_conditionals = IndexConditionals(std::move(conditionals));
    {
        util::Log() << "Writing " << indexed_conditionals.size()
                    << " conditional turn penalties...";
        // write conditional turn penalties into the restrictions file
        storage::io::FileWriter writer(conditional_penalties_filename,
                                       storage::io::FileWriter::GenerateFingerprint);
        extractor::serialization::write(writer, indexed_conditionals);
    }

    // write weight penalties per turn
    BOOST_ASSERT(turn_weight_penalties.size() == turn_duration_penalties.size());
    {
        storage::io::FileWriter writer(turn_weight_penalties_filename,
                                       storage::io::FileWriter::GenerateFingerprint);
        storage::serialization::write(writer, turn_weight_penalties);
    }

    {
        storage::io::FileWriter writer(turn_duration_penalties_filename,
                                       storage::io::FileWriter::GenerateFingerprint);
        storage::serialization::write(writer, turn_duration_penalties);
    }

    util::Log() << "Created " << entry_class_hash.data.size() << " entry classes and "
                << bearing_class_hash.data.size() << " Bearing Classes";

    util::Log() << "Writing Turn Lane Data to File...";
    {
        storage::io::FileWriter writer(turn_lane_data_filename,
                                       storage::io::FileWriter::GenerateFingerprint);

        std::vector<util::guidance::LaneTupleIdPair> lane_data(lane_data_map.data.size());
        // extract lane data sorted by ID
        for (auto itr : lane_data_map.data)
            lane_data[itr.second] = itr.first;

        storage::serialization::write(writer, lane_data);
    }
    util::Log() << "done.";

    files::writeTurnData(turn_data_filename, turn_data_container);

    util::Log() << "Generated " << m_edge_based_node_segments.size() << " edge based node segments";
    util::Log() << "Node-based graph contains " << node_based_edge_counter << " edges";
    util::Log() << "Edge-expanded graph ...";
    util::Log() << "  contains " << m_edge_based_edge_list.size() << " edges";
    util::Log() << "  skips " << restricted_turns_counter << " turns, "
                                                             "defined by "
                << node_restriction_map.Size() << " restrictions";
    util::Log() << "  skips " << skipped_uturns_counter << " U turns";
    util::Log() << "  skips " << skipped_barrier_turns_counter << " turns over barriers";
}

std::vector<ConditionalTurnPenalty>
EdgeBasedGraphFactory::IndexConditionals(std::vector<Conditional> &&conditionals) const
{
    boost::unordered_multimap<std::pair<NodeID, NodeID>, ConditionalTurnPenalty *> index;

    // build and index of all conditional restrictions
    for (auto &conditional : conditionals)
        index.insert(std::make_pair(std::make_pair(conditional.from_node, conditional.to_node),
                                    &conditional.penalty));

    std::vector<ConditionalTurnPenalty> indexed_restrictions;

    for (auto const &edge : m_edge_based_edge_list)
    {
        auto const range = index.equal_range(std::make_pair(edge.source, edge.target));
        for (auto itr = range.first; itr != range.second; ++itr)
        {
            itr->second->turn_offset = edge.data.turn_id;
            indexed_restrictions.push_back(*itr->second);
        }
    }

    return indexed_restrictions;
}

EdgeBasedGraphFactory::GeometryInfo EdgeBasedGraphFactory::GetGeometryInfo(
    NodeID start_node, EdgeID edge, const EdgeData &edge_data) const
{
    BOOST_ASSERT(!edge_data.reversed);

    NodeID target_node = m_node_based_graph.GetTarget(edge);

    GeometryInfo info;
    info.osm_way_id = edge_data.osm_way_id;

    if (m_compressed_edge_container.HasEntryForID(edge))
    {
        auto const &via_nodes = m_compressed_edge_container.GetBucketReference(edge);

        std::vector<NodeID> nodes;
        nodes.push_back(start_node);

        for (auto const &iter : via_nodes)
            if (nodes.back() != iter.node_id)
                nodes.push_back(iter.node_id);

        if (nodes.back() != target_node)
            nodes.push_back(target_node);

        NodeID last_node = SPECIAL_NODEID;
        for (size_t i = 1; i < nodes.size(); ++i)
        {
            NodeID node1 = nodes[i - 1];
            NodeID node2 = nodes[i];

            const EdgeID tmp_edge = m_nbg_uncompressed.FindEdge(node1, node2);
            if (tmp_edge == SPECIAL_EDGEID)
            {
                util::Log(logWARNING) << "Can't find edge " << node1 << " -> " << node2;
                throw util::exception("Invalid data.");
            }

            info.AddNode(m_osm_node_ids[node1]);
            last_node = node2;
        }

        info.AddNode(m_osm_node_ids[last_node]);
    }
    else
    {
        info.AddNode(m_osm_node_ids[start_node]);
        info.AddNode(m_osm_node_ids[target_node]);
    }

    return info;
}

std::vector<util::guidance::BearingClass> EdgeBasedGraphFactory::GetBearingClasses() const
{
    std::vector<util::guidance::BearingClass> result(bearing_class_hash.data.size());
    for (const auto &pair : bearing_class_hash.data)
    {
        BOOST_ASSERT(pair.second < result.size());
        result[pair.second] = pair.first;
    }
    return result;
}

const std::vector<BearingClassID> &EdgeBasedGraphFactory::GetBearingClassIds() const
{
    return bearing_class_by_node_based_node;
}

std::vector<BearingClassID> &EdgeBasedGraphFactory::GetBearingClassIds()
{
    return bearing_class_by_node_based_node;
}

std::vector<util::guidance::EntryClass> EdgeBasedGraphFactory::GetEntryClasses() const
{
    std::vector<util::guidance::EntryClass> result(entry_class_hash.data.size());
    for (const auto &pair : entry_class_hash.data)
    {
        BOOST_ASSERT(pair.second < result.size());
        result[pair.second] = pair.first;
    }
    return result;
}

} // namespace extractor
} // namespace osrm
