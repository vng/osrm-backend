//  This class constructs the edge-expanded routing graph

#ifndef EDGE_BASED_GRAPH_FACTORY_HPP_
#define EDGE_BASED_GRAPH_FACTORY_HPP_

#include "extractor/compressed_edge_container.hpp"
#include "extractor/conditional_turn_penalty.hpp"
#include "extractor/edge_based_edge.hpp"
#include "extractor/edge_based_node_segment.hpp"
#include "extractor/extraction_turn.hpp"
#include "extractor/guidance/turn_analysis.hpp"
#include "extractor/guidance/turn_instruction.hpp"
#include "extractor/guidance/turn_lane_types.hpp"
#include "extractor/nbg_to_ebg.hpp"
#include "extractor/node_data_container.hpp"
#include "extractor/original_edge_data.hpp"
#include "extractor/packed_osm_ids.hpp"
#include "extractor/query_node.hpp"
#include "extractor/restriction_index.hpp"
#include "extractor/way_restriction_map.hpp"

#include "util/concurrent_id_map.hpp"
#include "util/deallocating_vector.hpp"
#include "util/guidance/bearing_class.hpp"
#include "util/guidance/entry_class.hpp"
#include "util/name_table.hpp"
#include "util/node_based_graph.hpp"
#include "util/typedefs.hpp"

#include "storage/io.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace osrm
{
namespace extractor
{

class ScriptingEnvironment;

namespace lookup
{
#pragma pack(push, 1)
struct TurnIndexBlock
{
    NodeID from_id;
    NodeID via_id;
    NodeID to_id;
};
#pragma pack(pop)
static_assert(std::is_trivial<TurnIndexBlock>::value, "TurnIndexBlock is not trivial");
static_assert(sizeof(TurnIndexBlock) == 12, "TurnIndexBlock is not packed correctly");
} // namespace lookup

struct NodeBasedGraphToEdgeBasedGraphMappingWriter; // fwd. decl

class EdgeBasedGraphFactory
{
  public:
    EdgeBasedGraphFactory(const EdgeBasedGraphFactory &) = delete;
    EdgeBasedGraphFactory &operator=(const EdgeBasedGraphFactory &) = delete;

    struct GeometryInfo
    {
        using NodesList = std::vector<OSMNodeID>;

        GeometryInfo() : osm_way_id(SPECIAL_OSM_WAYID) {}

        void AddNode(OSMNodeID osm_id) { nodes.emplace_back(osm_id); }

        template <typename TReader> void LoadT(TReader &reader)
        {
            std::uint32_t num = 0;
            reader.Read((char *)&osm_way_id, sizeof(OSMWayID));
            reader.Read((char *)&num, sizeof(num));
            nodes.resize(num);
            reader.Read(nodes.data(), sizeof(OSMNodeID) * num);
        }

        void Load(std::ifstream &stream)
        {
            std::uint32_t num = 0;
            stream.read((char *)&osm_way_id, sizeof(OSMWayID));
            stream.read((char *)&num, sizeof(num));
            nodes.resize(num);
            stream.read((char *)nodes.data(), sizeof(OSMNodeID) * num);
        }

        void Write(std::ofstream &stream)
        {
            const std::uint32_t num = nodes.size();
            stream.write((char *)&osm_way_id, sizeof(OSMWayID));
            stream.write((char *)&num, sizeof(num));
            for (std::uint32_t i = 0; i < num; ++i)
                stream.write((char *)&(nodes[i]), sizeof(OSMNodeID));
        }

        OSMWayID osm_way_id;
        NodesList nodes;
    };

    class GeometryInfoContainer
    {
      public:
        using GeometryList = std::vector<GeometryInfo>;

        GeometryInfo const &operator[](size_t idx) const
        {
            BOOST_ASSERT(idx < m_data.size());
            return m_data[idx];
        }

        void reserve(size_t new_size) { m_data.reserve(new_size); }
        size_t size() const { return m_data.size(); }

        template <class T>
        void Add(T &&info) { m_data.push_back(std::forward<T>(info)); }

        void Load(std::string const &fileName)
        {
            std::ifstream in_file(fileName);
            if (!in_file.is_open())
                throw util::exception(std::string("Can't open input file ") + fileName);

            uint32_t num = 0;
            in_file.read((char *)&num, sizeof(num));
            m_data.resize(num);
            for (uint32_t i = 0; i < num; ++i)
                m_data[i].Load(in_file);

            in_file.close();
        }

        void Save(std::string const &fileName)
        {
            std::ofstream out_file(fileName);
            if (!out_file.is_open())
                throw util::exception(std::string("Can't open output file ") + fileName);

            uint32_t const num = m_data.size();
            out_file.write((char *)&num, sizeof(num));
            for (uint32_t i = 0; i < num; ++i)
                m_data[i].Write(out_file);

            out_file.close();
        }

      private:
        GeometryList m_data;
    };

    explicit EdgeBasedGraphFactory(const util::NodeBasedDynamicGraph &node_based_graph,
                                   const util::NodeBasedDynamicGraph &nbg_uncompressed,
                                   const extractor::PackedOSMIDs &osm_node_ids,
                                   EdgeBasedNodeDataContainer &node_data_container,
                                   const CompressedEdgeContainer &compressed_edge_container,
                                   const std::unordered_set<NodeID> &barrier_nodes,
                                   const std::unordered_set<NodeID> &traffic_lights,
                                   const std::vector<util::Coordinate> &coordinates,
                                   const util::NameTable &name_table,
                                   const std::unordered_set<EdgeID> &segregated_edges,
                                   guidance::LaneDescriptionMap &lane_description_map);

    void Run(ScriptingEnvironment &scripting_environment,
             const std::string &turn_data_filename,
             const std::string &turn_lane_data_filename,
             const std::string &turn_weight_penalties_filename,
             const std::string &turn_duration_penalties_filename,
             const std::string &turn_penalties_index_filename,
             const std::string &cnbg_ebg_mapping_path,
             const std::string &conditional_penalties_filename,
             const std::string &geometry_info_filename,
             const RestrictionMap &node_restriction_map,
             const ConditionalRestrictionMap &conditional_restriction_map,
             const WayRestrictionMap &way_restriction_map);

    // The following get access functions destroy the content in the factory
    void GetEdgeBasedEdges(util::DeallocatingVector<EdgeBasedEdge> &edges);
    void GetEdgeBasedNodeSegments(std::vector<EdgeBasedNodeSegment> &nodes);
    void GetStartPointMarkers(std::vector<bool> &node_is_startpoint);
    void GetEdgeBasedNodeWeights(std::vector<EdgeWeight> &output_node_weights);

    // These access functions don't destroy the content
    const std::vector<BearingClassID> &GetBearingClassIds() const;
    std::vector<BearingClassID> &GetBearingClassIds();
    std::vector<util::guidance::BearingClass> GetBearingClasses() const;
    std::vector<util::guidance::EntryClass> GetEntryClasses() const;

    std::uint64_t GetNumberOfEdgeBasedNodes() const;

    // Basic analysis of a turn (u --(e1)-- v --(e2)-- w)
    // with known angle.
    // Handles special cases like u-turns and roundabouts
    // For basic turns, the turn based on the angle-classification is returned
    guidance::TurnInstruction AnalyzeTurn(const NodeID u,
                                          const EdgeID e1,
                                          const NodeID v,
                                          const EdgeID e2,
                                          const NodeID w,
                                          const double angle) const;

  private:
    using EdgeData = util::NodeBasedDynamicGraph::EdgeData;

    struct Conditional
    {
        // the edge based nodes allow for a unique identification of conditionals
        NodeID from_node;
        NodeID to_node;
        ConditionalTurnPenalty penalty;
    };

    // assign the correct index to the penalty value stored in the conditional
    std::vector<ConditionalTurnPenalty>
    IndexConditionals(std::vector<Conditional> &&conditionals) const;

    //! maps index from m_edge_based_node_list to ture/false if the node is an entry point to the
    //! graph
    std::vector<bool> m_edge_based_node_is_startpoint;

    //! node weights that indicate the length of the segment (node based) represented by the
    //! edge-based node
    std::vector<EdgeWeight> m_edge_based_node_weights;

    //! list of edge based nodes (compressed segments)
    std::vector<EdgeBasedNodeSegment> m_edge_based_node_segments;
    EdgeBasedNodeDataContainer &m_edge_based_node_container;
    const extractor::PackedOSMIDs &m_osm_node_ids;
    util::DeallocatingVector<EdgeBasedEdge> m_edge_based_edge_list;
    GeometryInfoContainer m_ebn_geometry_info;

    // The number of edge-based nodes is mostly made up out of the edges in the node-based graph.
    // Any edge in the node-based graph represents a node in the edge-based graph. In addition, we
    // add a set of artificial edge-based nodes into the mix to model via-way turn restrictions.
    // See https://github.com/Project-OSRM/osrm-backend/issues/2681#issuecomment-313080353 for
    // reference
    std::uint64_t m_number_of_edge_based_nodes;

    const std::vector<util::Coordinate> &m_coordinates;
    const util::NodeBasedDynamicGraph &m_node_based_graph;

    /// @todo This graph is needed for validation checks only.
    /// Can be removed in production with the NodeBasedGraphFactory::GetUncompressedGraph().
    const util::NodeBasedDynamicGraph &m_nbg_uncompressed;

    const std::unordered_set<NodeID> &m_barrier_nodes;
    const std::unordered_set<NodeID> &m_traffic_lights;
    const CompressedEdgeContainer &m_compressed_edge_container;

    const util::NameTable &name_table;
    const std::unordered_set<EdgeID> &segregated_edges;
    guidance::LaneDescriptionMap &lane_description_map;

    // In the edge based graph, any traversable (non reversed) edge of the node-based graph forms a
    // node of the edge-based graph. To be able to name these nodes, we loop over the node-based
    // graph and create a mapping from edges (node-based) to nodes (edge-based). The mapping is
    // essentially a prefix-sum over all previous non-reversed edges of the node-based graph.
    unsigned LabelEdgeBasedNodes();

    // During the generation of the edge-expanded nodes, we need to also generate duplicates that
    // represent state during via-way restrictions (see
    // https://github.com/Project-OSRM/osrm-backend/issues/2681#issuecomment-313080353). Access to
    // the information on what to duplicate and how is provided via the way_restriction_map
    std::vector<NBGToEBG> GenerateEdgeExpandedNodes(const WayRestrictionMap &way_restriction_map);

    // Edge-expanded edges are generate for all valid turns. The validity can be checked via the
    // restriction maps
    void GenerateEdgeExpandedEdges(ScriptingEnvironment &scripting_environment,
                                   const std::string &original_edge_data_filename,
                                   const std::string &turn_lane_data_filename,
                                   const std::string &turn_weight_penalties_filename,
                                   const std::string &turn_duration_penalties_filename,
                                   const std::string &turn_penalties_index_filename,
                                   const std::string &conditional_turn_penalties_filename,
                                   const RestrictionMap &node_restriction_map,
                                   const ConditionalRestrictionMap &conditional_restriction_map,
                                   const WayRestrictionMap &way_restriction_map);

    NBGToEBG InsertEdgeBasedNode(const NodeID u, const NodeID v);

    GeometryInfo GetGeometryInfo(NodeID start_node, EdgeID edge, const EdgeData &edge_data) const;

    std::size_t restricted_turns_counter;
    std::size_t skipped_uturns_counter;
    std::size_t skipped_barrier_turns_counter;

    // mapping of node-based edges to edge-based nodes
    std::vector<NodeID> nbe_to_ebn_mapping;
    util::ConcurrentIDMap<util::guidance::BearingClass, BearingClassID> bearing_class_hash;
    std::vector<BearingClassID> bearing_class_by_node_based_node;
    util::ConcurrentIDMap<util::guidance::EntryClass, EntryClassID> entry_class_hash;
};
} // namespace extractor
} // namespace osrm

#endif /* EDGE_BASED_GRAPH_FACTORY_HPP_ */
