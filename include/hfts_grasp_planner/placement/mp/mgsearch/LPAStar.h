#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <boost/heap/fibonacci_heap.hpp>

#include <hfts_grasp_planner/placement/mp/mgsearch/SearchCommon.h>

/**
 *  Defines Lifelong Planning A* and needed data structures
 */
namespace placement
{
namespace mp
{
namespace mgsearch
{
namespace lpastar
{
// Key for priority queue
typedef std::pair<double, double> Key;
bool operator<(const Key& a, const Key& b);

// Class that encapsulates LPA* algorithm and needed data structures
template <typename G>
class LPAStarAlgorithm
{
protected:
  /****** Algorithm specific typedefs and structs *******/
  struct PQElement
  {
    unsigned int v;
    Key key;
    PQElement(unsigned int _v, std::pair<double, double> _key) : v(_v), key(_key)
    {
    }
  };

  struct PQElementCompare
  {
    // Does b have priority over a?
    bool operator()(const PQElement& a, const PQElement& b) const
    {
      return b.key < a.key;
    }
  };

  // Priority queue type used by lpastar
  typedef boost::heap::fibonacci_heap<PQElement, boost::heap::compare<PQElementCompare>> PQ;

  // compute key for priority queue
  Key computeKey(double g, double h, double rhs) const
  {
    double g_p = std::min(g, rhs);
    return {g_p + h, g_p};
  }

  struct VertexData
  {
    const unsigned int v;  // id
    double g;              // g value
    double h;              // heuristic value
    double rhs;            // right-hand-side value of Bellman equation
    unsigned int p;        // parent id
    bool in_pq;            // true if vertex is in PQ
    typename PQ::handle_type pq_handle;
    VertexData(unsigned int v_, double g_, double h_, double rhs_, unsigned int p_)
      : v(v_), g(g_), h(h_), rhs(rhs_), p(p_), in_pq(false)
    {
    }
  };

  typedef std::unordered_map<unsigned int, VertexData> VertexDataMap;

public:
  /**
   * Create a new instance of the LPAStarAlgorithm.
   * @param graph: The graph to operate on. The reference is stored in a member
   * variable. Hence, an instance of this class should only live as long as the
   * graph.
   */
  LPAStarAlgorithm(const G& graph, bool lazy = true) : _graph(graph), _lazy(lazy)
  {
    _v_start = _graph.getStartNode();
    // initialize start state
    if (_graph.checkValidity(_v_start))
    {
      VertexData start_node(_v_start, 0.0, _graph.heuristic(_v_start), 0.0, _v_start);
      _vertex_data.emplace(std::make_pair(_v_start, start_node));
      _vertex_data.at(_v_start).pq_handle =
          _pq.push(PQElement(_v_start, computeKey(start_node.g, start_node.h, start_node.rhs)));
      // initialize result
      _result.solved = false;
      _result.path.clear();
      _result.path_cost = std::numeric_limits<double>::infinity();
      _result.goal_cost = std::numeric_limits<double>::infinity();
      _result.goal_node = _v_start;
      _goal_key = computeKey(std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::infinity());
    }
  }

  ~LPAStarAlgorithm() = default;

  // Struct to communicate a cost change to the algorithm
  struct EdgeChange
  {
    unsigned int u;  // edge goes from u to v
    unsigned int v;
    double old_cost;  // cost prior to the update
  };

  /**
   * Update algorithm state to reflect edge weight changes.
   * @param edge_changes: the edge changes
   */
  void updateEdges(const std::vector<EdgeChange>& edge_changes)
  {
    utils::ScopedProfiler("LPAStarAlgorithm::updateEdges");
    for (const EdgeChange& ec : edge_changes)
    {
      VertexData& u_data = getVertexData(ec.u);
      VertexData& v_data = getVertexData(ec.v);
      double new_cost = _graph.getEdgeCost(ec.u, ec.v, _lazy);
      if (ec.old_cost > new_cost)  // did edge get cheaper?
      {
        handleCostDecrease(u_data, v_data);
      }
      else if (ec.v != _v_start and v_data.p == ec.u)
      {
        handleCostIncrease(u_data, v_data);
      }
    }
  }

  /**
   * Compute the shortest path given the current algorithm state.
   * @param result: Struct that will contain the search result
   */
  void computeShortestPath(SearchResult& result)
  {
    utils::ScopedProfiler("LPAStarAlgorithm::computeShortestPath");
    // main iteration
    // keep repeating as long as
    // 1. there are inconsistent nodes with keys less than _goal_key
    //    (_goal_key is initialized with inf or from previous run)
    // 2. or the goal responsible for _goal_key is not locally consistent (_result.solved)
    // 3. and there are still inconsistent nodes to work on (to capture infeasible queries)
    while ((_pq.top().key < _goal_key or not _result.solved) and not _pq.empty())
    {
      assert(_pq.top().key <= _goal_key);
      PQElement current_el(_pq.top());
      _pq.pop();
      // get vertex data
      VertexData& u_data = getVertexData(current_el.v);
      // check if us is overconsistent?
      if (u_data.g > u_data.rhs)
      {  // make it consistent
        u_data.g = u_data.rhs;
        updateVertexKey(u_data);
        // update neighbors
        auto [iter, end_iter] = _graph.getSuccessors(u_data.v, _lazy);
        for (; iter != end_iter; ++iter)
        {
          VertexData& v_data = getVertexData(*iter);
          handleCostDecrease(u_data, v_data);
        }
      }
      else
      {
        // u is underconsistent
        u_data.g = std::numeric_limits<double>::infinity();
        auto [iter, end_iter] = _graph.getSuccessors(u_data.v, _lazy);
        for (; iter != end_iter; ++end_iter)
        {
          VertexData& v_data = getVertexData(*iter);
          handleCostIncrease(u_data, v_data);
        }
        updateVertexKey(u_data);
      }
    }
    // the _result keeps track of reached goal nodes and kept across runs of this algorithm
    result = _result;
    // extract path
    if (result.solved)
    {
      extractPath<VertexDataMap>(_v_start, _vertex_data, result);
    }
  }

protected:
  // VertexData
  PQ _pq;
  VertexDataMap _vertex_data;
  SearchResult _result;
  Key _goal_key;
  const G& _graph;
  bool _lazy;
  unsigned int _v_start;

  /**
   * Handle a cost increase from u to v.
   * This may either be due to an increased edge cost (u, v) or due to u itself being more expensively to reach.
   * Updates v_data accordingly.
   */
  void handleCostIncrease(VertexData& u_data, VertexData& v_data)
  {
    // going over u to v got more expensive
    // test whether v needs to care about that. if so, look for a new parent of v
    if (v_data.p == u_data.v)
    {
      auto [iter, end] = _graph.getPredecessors(v_data.v, _lazy);
      for (; iter != end; ++iter)
      {
        unsigned int s = *iter;
        VertexData& s_data = getVertexData(s);
        double rhs = s_data.g + _graph.getEdgeCost(s, v_data.v, _lazy);
        if (rhs < v_data.rhs)  // v is cheaper to reach through s
        {
          v_data.rhs = rhs;
          v_data.p = s;
        }
      }
      updateVertexKey(v_data);
    }
  }

  /**
   * Handle cost decrease from u to v.
   * This may either be due to a decreased edge cost (u, v) or due to g(u) being decreased.
   * Updates v_data accordingly.
   */
  void handleCostDecrease(VertexData& u_data, VertexData& v_data)
  {
    double edge_cost = _graph.getEdgeCost(u_data.v, v_data.v);
    if (v_data.rhs > u_data.g + edge_cost)  // update u if it can now be reached at a lower cost
    {
      v_data.p = u_data.v;
      v_data.rhs = u_data.g + edge_cost;
      updateVertexKey(v_data);
    }
  }

  /**
   * Update the v's key in _pq and remove if needed.
   * If v is a goal and a new solution, alsp update _goal_key.
   */
  void updateVertexKey(VertexData& v_data)
  {
    // 1. update pq membership
    if (v_data.g != v_data.rhs and v_data.in_pq)
    {
      auto old_key = (*v_data.pq_handle).key;
      auto new_key = computeKey(v_data.g, v_data.h, v_data.rhs);
      (*v_data.pq_handle).key = new_key;
      if (old_key < new_key)
      {
        _pq.decrease(v_data.pq_handle);  // priority has decreased
      }
      else if (old_key > new_key)
      {
        _pq.increase(v_data.pq_handle);  // priority has increased
      }
    }
    else if (v_data.g != v_data.rhs and not v_data.in_pq)
    {
      // add v to PQ
      v_data.pq_handle = _pq.push(PQElement(v_data.v, computeKey(v_data.g, v_data.h, v_data.rhs)));
      v_data.in_pq = true;
    }
    else if (v_data.g == v_data.rhs and v_data.in_pq)
    {
      // v is no longer inconsistent, so remove it from _pq
      (*v_data.pq_handle).key = Key(0.0, 0.0);
      _pq.increase(v_data.pq_handle);
      _pq.pop();
      v_data.in_pq = false;
    }
    // 2. if v is a goal, update goal key if better
    if (_graph.isGoal(v_data.v))
    {
      double goal_cost = _graph.getGoalCost(v_data.v);
      Key v_goal_key = computeKey(v_data.g, goal_cost, v_data.rhs);
      if (v_goal_key < _goal_key)
      {
        _goal_key = v_goal_key;
        _result.goal_node = v_data.v;
        _result.goal_cost = goal_cost;
        _result.path_cost = v_data.g;
        _result.solved = v_data.g == v_data.rhs;
      }
    }
  }

  VertexData& getVertexData(unsigned int v)
  {
    auto iter = _vertex_data.find(v);
    if (iter == _vertex_data.end())
    {
      _vertex_data.insert({v, VertexData(v, std::numeric_limits<double>::infinity(), _graph.heuristic(v),
                                         std::numeric_limits<double>::infinity(), v)});
    }
    return _vertex_data.at(v);
  }
};

/**
 * LPA* search algorithm.
 * The template parameter G needs to be of a type implementing the
 * GraspAgnosticGraph interface specified in Graphs.h.
 * @param graph: The graph to search a path on.
 * @param result: Struct that will contain the search result
 */
template <typename G>
void lpaStarSearch(const G& graph, SearchResult& result)
{
  utils::ScopedProfiler("lpaStarSearch");
  LPAStarAlgorithm algorithm(graph, false);
  algorithm.computeShortestPath(result);
}

}  // namespace lpastar
}  // namespace mgsearch
}  // namespace mp
}  // namespace placement