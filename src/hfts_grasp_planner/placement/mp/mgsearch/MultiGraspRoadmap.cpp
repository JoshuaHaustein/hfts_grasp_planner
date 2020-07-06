#include <Eigen/Core>
#include <hfts_grasp_planner/external/halton/halton.hpp>
#include <hfts_grasp_planner/placement/mp/mgsearch/MultiGraspRoadmap.h>

using namespace placement::mp::mgsearch;

StateSpace::~StateSpace() = default;

EdgeCostComputer::~EdgeCostComputer() = default;

IntegralEdgeCostComputer::IntegralEdgeCostComputer(StateSpacePtr state_space, double step_size)
    : _state_space(state_space)
    , _step_size(step_size)
{
}

IntegralEdgeCostComputer::~IntegralEdgeCostComputer() = default;

double IntegralEdgeCostComputer::integrateCosts(const Config& a, const Config& b,
    const std::function<double(const Config&)>& cost_fn) const
{
    assert(a.size() == b.size());
    Eigen::Map<const Eigen::VectorXd> avec(a.data(), a.size());
    Eigen::Map<const Eigen::VectorXd> bvec(b.data(), b.size());
    Eigen::VectorXd delta = bvec - avec;
    Config q(delta.size());
    Eigen::Map<Eigen::VectorXd> qvec(q.data(), q.size());
    double norm = delta.norm();
    if (norm == 0.0) {
        return 0.0;
    }
    delta /= norm;
    // iterate over path
    double integral_cost = 0.0;
    unsigned int num_steps = std::ceil(norm / _step_size);
    double progress = 0.0;
    for (size_t t = 0; t < num_steps; ++t) {
        qvec = progress * delta + avec;
        double step_size = std::min(_step_size, norm - progress);
        progress += step_size;
        double dc = cost_fn(q); // qvec is a view on q's data
        if (std::isinf(dc))
            return INFINITY;
        integral_cost += dc * step_size;
    }
    return integral_cost;
}

double IntegralEdgeCostComputer::lowerBound(const Config& a, const Config& b) const
{
    return _state_space->distance(a, b);
}

double IntegralEdgeCostComputer::cost(const Config& a, const Config& b) const
{
    auto fn = std::bind(&StateSpace::cost, _state_space, std::placeholders::_1);
    return integrateCosts(a, b, fn);
}

double IntegralEdgeCostComputer::cost(const Config& a, const Config& b, unsigned int grasp_id) const
{
    auto fn = std::bind(&StateSpace::conditional_cost, _state_space, std::placeholders::_1, grasp_id);
    return integrateCosts(a, b, fn);
}

CostToGoHeuristic::~CostToGoHeuristic() = default;

// double distanceFn(const Roadmap::NodePtr& a, const Roadmap::NodePtr& b)
// {
//     return cSpaceDistance(a->config, b->config);
// }

Roadmap::Edge::Edge(Roadmap::NodePtr a, Roadmap::NodePtr b, double bc)
    : base_cost(bc)
    , base_evaluated(false)
    , node_a(a)
    , node_b(b)
{
}

Roadmap::NodePtr Roadmap::Edge::getNeighbor(NodePtr n) const
{
    auto a = node_a.lock();
    if (a != nullptr and a->uid != n->uid)
        return a;
    auto b = node_b.lock();
    assert(b == nullptr or b->uid != n->uid);
    return b;
}

double Roadmap::Edge::getBestKnownCost(unsigned int gid) const
{
    auto iter = conditional_costs.find(gid);
    if (iter != conditional_costs.end()) {
        return iter->second;
    }
    return base_cost;
}

Roadmap::Logger::Logger() = default;

Roadmap::Logger::~Logger()
{
    _roadmap_fs.close();
    _log_fs.close();
}

void Roadmap::Logger::setLogPath(const std::string& roadmap_file, const std::string& log_file)
{
    if (_roadmap_fs.is_open()) {
        _roadmap_fs.close();
    }
    if (_log_fs.is_open()) {
        _log_fs.close();
    }
    if (!roadmap_file.empty()) {
        _roadmap_fs.open(roadmap_file, std::ios::out);
    }
    if (!log_file.empty()) {
        _log_fs.open(log_file, std::ios::out);
    }
}

void Roadmap::Logger::newNode(NodePtr node)
{
    if (_roadmap_fs.is_open()) {
        // a node is stored as a single line: id, dim, x1, x2, ..., xdim\n
        _roadmap_fs << node->uid << ", ";
        _roadmap_fs << node->config.size();
        for (auto ci : node->config) {
            _roadmap_fs << ", " << ci;
        }
        // _roadmap_fs << "\n";
        _roadmap_fs << std::endl;
    }
}

void Roadmap::Logger::nodeValidityChecked(NodePtr node, bool bval)
{
    if (_log_fs.is_open()) {
        // log that the validity of node has been checked. format: VALBASE, id, bval\n
        _log_fs << "VAL_BASE, " << node->uid << ", " << bval << "\n";
    }
}

void Roadmap::Logger::nodeValidityChecked(NodePtr node, unsigned int grasp_id, bool bval)
{
    if (_log_fs.is_open()) {
        // log that the validity of node has been checked. format: VALGRASP, id, grasp_id, bval\n
        _log_fs << "VAL_GRASP, " << node->uid << ", " << grasp_id << ", " << bval << "\n";
    }
}

void Roadmap::Logger::edgeCostChecked(NodePtr a, NodePtr b, double cost)
{
    if (_log_fs.is_open()) {
        // log that a cost of the edge connecting a and b has been computed
        // format: EDGE_COST, aid, bid, cost\n
        _log_fs << "EDGE_COST, " << a->uid << ", " << b->uid << ", " << cost << "\n";
    }
}

void Roadmap::Logger::edgeCostChecked(NodePtr a, NodePtr b, unsigned int grasp_id, double cost)
{
    if (_log_fs.is_open()) {
        // log that a cost of the edge connecting a and b has been computed
        // format: EDGE_COST_GRASP, aid, bid, grasp_id, cost\n
        _log_fs << "EDGE_COST_GRASP, " << a->uid << ", " << b->uid << ", " << grasp_id << ", " << cost << "\n";
    }
}

Roadmap::Roadmap(StateSpacePtr state_space, EdgeCostComputerPtr cost_computer, unsigned int batch_size,
    const std::string& log_roadmap_path, const std::string& log_path)
    : _state_space(state_space)
    , _si(state_space->getSpaceInformation())
    , _cost_computer(cost_computer)
    , _batch_size(batch_size)
    , _node_id_counter(0)
    , _halton_seq_id(0)
    , _densification_gen(0)
{
    assert(_si.lower.size() == _si.upper.size() and _si.lower.size() == _si.dimension);
    _logger.setLogPath(log_roadmap_path, log_path);
    // _nn.setDistanceFunction(distanceFn);
    _nn.setDistanceFunction([this](const Roadmap::NodePtr& a, const Roadmap::NodePtr& b) { return _state_space->distance(a->config, b->config); });
    // compute gamma_prm - a constant used to compute the radius for adjacency
    // we need the measure of X_free for this, we approximate it by the measure of X
    double mu = 1.0;
    for (unsigned int i = 0; i < _si.dimension; ++i) {
        mu *= _si.upper[i] - _si.lower[i];
    }
    // xi is the measure of a dof-dimensional unit ball
    double xi = pow(M_PI, _si.dimension / 2.0) / tgamma(_si.dimension / 2.0 + 1.0);
    // finally compute gamma_prm. See Sampling-based algorithms for optimal motion planning by Karaman and Frazzoli
    _gamma_prm = 2.0 * pow((1.0 + 1.0 / _si.dimension) * mu / xi, 1.0 / _si.dimension);
    // now densify the roadmap
    densify(batch_size);
}

Roadmap::~Roadmap() = default;

void Roadmap::densify()
{
    densify(_batch_size);
}

void Roadmap::densify(unsigned int batch_size)
{
    assert(batch_size > 0);
    double* new_samples = halton::halton_sequence(_halton_seq_id, _halton_seq_id + batch_size - 1, _si.dimension);
    _halton_seq_id += batch_size;
    double* config_pointer = new_samples;
    Config config(_si.dimension);
    for (unsigned int id = 0; id < batch_size; ++id) {
        // TODO add random noise to config
        config.assign(config_pointer, config_pointer + _si.dimension);
        scaleToLimits(config);
        NodePtr new_node = addNode(config).lock();
        assert(new_node->config.size() == _si.dimension);
        config_pointer += _si.dimension;
    }
    delete[] new_samples;
    _densification_gen += 1;
}

void Roadmap::setLogging(const std::string& roadmap_path, const std::string& log_path)
{
    _logger.setLogPath(roadmap_path, log_path);
}

Roadmap::NodePtr Roadmap::getNode(unsigned int node_id) const
{
    auto iter = _nodes.find(node_id);
    if (iter == _nodes.end()) {
        return nullptr;
    }
    auto ptr = iter->second.lock();
    assert(ptr != nullptr);
    return ptr;
}

Roadmap::NodeWeakPtr Roadmap::addNode(const Config& config)
{
    NodePtr new_node = std::shared_ptr<Node>(new Node(_node_id_counter++, config));
    _nn.add(new_node);
    _nodes.insert(std::make_pair(new_node->uid, new_node));
    _logger.newNode(new_node);
    return new_node;
}

void Roadmap::updateAdjacency(NodePtr node)
{
    // update the node's adjacency
    if (node->densification_gen != _densification_gen) {
        // radius computed according to RRT*/PRM* paper
        double r = _gamma_prm * pow(log(_nn.size()) / _nn.size(), 1.0 / _si.dimension);
        std::vector<NodePtr> neighbors;
        _nn.nearestR(node, r, neighbors);
        // add new edges, keep old ones
        for (auto& neigh : neighbors) {
            // check whether we already have this edge
            auto edge_iter = node->edges.find(neigh->uid);
            if (edge_iter == node->edges.end() and neigh != node) {
                // if not, create a new edge
                double bc = _cost_computer->lowerBound(node->config, neigh->config);
                auto new_edge = std::make_shared<Edge>(node, neigh, bc);
                node->edges.insert(std::make_pair(neigh->uid, new_edge));
                neigh->edges.insert(std::make_pair(node->uid, new_edge));
            }
        }
        node->densification_gen = _densification_gen;
    }
    // clean up edges that are no longer needed because they are invalid
    for (auto edge_iter = node->edges.begin(); edge_iter != node->edges.end();) {
        auto edge = edge_iter->second;
        if (edge->base_evaluated && std::isinf(edge->base_cost)) {
            // edge is invalid, so let's remove it
            edge_iter = node->edges.erase(edge_iter);
        } else {
            ++edge_iter;
        }
    }
}

bool Roadmap::isValid(NodeWeakPtr inode)
{
    if (inode.expired())
        return false;
    auto node = inode.lock();
    if (!node->initialized) {
        bool valid = _state_space->isValid(node->config);
        _logger.nodeValidityChecked(node, valid);
        // check validity
        if (not valid) {
            // in case of the node being invalid, remove it
            deleteNode(node);
            return false;
        }
    }
    node->initialized = true;
    return true;
}

bool Roadmap::isValid(NodeWeakPtr wnode, unsigned int grasp_id)
{
    bool base_valid = isValid(wnode);
    if (base_valid) {
        // check validity for the given grasp
        NodePtr node = wnode.lock();
        auto iter = node->conditional_validity.find(grasp_id);
        if (iter == node->conditional_validity.end()) {
            bool valid = _state_space->isValid(node->config, grasp_id, true);
            node->conditional_validity[grasp_id] = valid;
            _logger.nodeValidityChecked(node, grasp_id, valid);
            return valid;
        } else {
            return iter->second;
        }
    }
    return false;
}

std::pair<bool, double> Roadmap::computeCost(EdgePtr edge)
{
    if (edge->base_evaluated) {
        return { !std::isinf(edge->base_cost), edge->base_cost };
    }
    // we have to compute base cost
    NodePtr node_a = edge->node_a.lock();
    NodePtr node_b = edge->node_b.lock();
    assert(node_a);
    assert(node_b);
    edge->base_cost = _cost_computer->cost(node_a->config, node_b->config);
    edge->base_evaluated = true;
    _logger.edgeCostChecked(node_a, node_b, edge->base_cost);
    return { !std::isinf(edge->base_cost), edge->base_cost };
}

std::pair<bool, double> Roadmap::computeCost(EdgeWeakPtr weak_edge)
{
    if (weak_edge.expired())
        return { false, INFINITY };
    auto edge = weak_edge.lock();
    return computeCost(edge);
}

std::pair<bool, double> Roadmap::computeCost(EdgePtr edge, unsigned int grasp_id)
{
    if (edge->base_evaluated and std::isinf(edge->base_cost)) {
        return { false, edge->base_cost };
    }
    auto iter = edge->conditional_costs.find(grasp_id);
    double cost;
    if (iter == edge->conditional_costs.end()) {
        NodePtr node_a = edge->node_a.lock();
        assert(node_a);
        NodePtr node_b = edge->node_b.lock();
        assert(node_b);
        cost = _cost_computer->cost(node_a->config, node_b->config, grasp_id);
        _logger.edgeCostChecked(node_a, node_b, grasp_id, cost);
        edge->conditional_costs.insert(std::make_pair(grasp_id, cost));
    } else {
        cost = iter->second;
    }
    return { not std::isinf(cost), cost };
}

void Roadmap::scaleToLimits(Config& config) const
{
    assert(config.size() == _si.dimension);
    for (unsigned int i = 0; i < _si.dimension; ++i) {
        config[i] = config[i] * (_si.upper[i] - _si.lower[i]) + _si.lower[i];
    }
}

void Roadmap::deleteNode(NodePtr node)
{
    _nn.remove(node);
    auto iter = _nodes.find(node->uid);
    assert(iter != _nodes.end());
    _nodes.erase(iter);
    // set all its edges to infinite cost;
    // if the neighboring node still exists, its edge map will be eventually updated in updateAdjacency(..)
    for (auto iter = node->edges.begin(); iter != node->edges.end(); ++iter) {
        // unsigned int edge_target_id = out_info.first;
        EdgePtr edge = iter->second;
        edge->base_evaluated = true;
        edge->base_cost = std::numeric_limits<double>::infinity();
    }
    node.reset();
}

/************************************** MultiGraspGoalSet **************************************/
MultiGraspGoalSet::MultiGraspGoalSet(RoadmapPtr roadmap)
    : _roadmap(roadmap)
{
}

MultiGraspGoalSet::~MultiGraspGoalSet() = default;

void MultiGraspGoalSet::addGoal(const MultiGraspMP::Goal& goal)
{
    _goals.insert(std::make_pair(goal.id, goal));
    auto new_node = _roadmap->addNode(goal.config).lock();
    assert(new_node);
    _goal_id_to_roadmap_id[goal.id] = new_node->uid;
    _roadmap_id_to_goal_id[new_node->uid] = goal.id;
}

placement::mp::MultiGraspMP::Goal MultiGraspGoalSet::getGoal(unsigned int gid) const
{
    auto goal_iter = _goals.find(gid);
    if (goal_iter == _goals.end()) {
        throw std::logic_error("There is no goal with id " + std::to_string(gid));
    }
    return goal_iter->second;
}

void MultiGraspGoalSet::removeGoal(unsigned int gid)
{
    auto goal_iter = _goals.find(gid);
    if (goal_iter != _goals.end()) {
        _goals.erase(goal_iter);
        // remove goal from goal_id_to_roadmap id map
        auto gid2rid_iter = _goal_id_to_roadmap_id.find(gid);
        assert(gid2rid_iter != _goal_id_to_roadmap_id.end());
        unsigned int rid = gid2rid_iter->second;
        _goal_id_to_roadmap_id.erase(gid2rid_iter);
        // remove inverse mapping
        auto rid2gid_iter = _roadmap_id_to_goal_id.find(rid);
        assert(rid2gid_iter != _roadmap_id_to_goal_id.end());
        assert(rid2gid_iter->second == gid);
        _roadmap_id_to_goal_id.erase(rid2gid_iter);
    }
}

void MultiGraspGoalSet::removeGoals(const std::vector<unsigned int>& goal_ids)
{
    for (unsigned int gid : goal_ids) {
        removeGoal(gid);
    }
}

bool MultiGraspGoalSet::isGoal(Roadmap::NodePtr node, unsigned int grasp_id)
{
    // a configuration is not a goal for a grasp if it is invalid
    if (!_roadmap->isValid(node, grasp_id))
        return false;
    // if valid, check whether this roadmap node is affiliated with a goal
    unsigned int goal_id;
    {
        auto iter = _roadmap_id_to_goal_id.find(node->uid);
        if (iter == _roadmap_id_to_goal_id.end())
            return false;
        goal_id = iter->second;
    }
    // check whether the goal is for the given grasp
    return grasp_id == _goals.at(goal_id).grasp_id;
}

bool MultiGraspGoalSet::isGoal(unsigned int node_id, unsigned int grasp_id)
{
    auto node_ptr = _roadmap->getNode(node_id);
    if (!node_ptr)
        return false;
    return isGoal(node_ptr, grasp_id);
}

std::pair<unsigned int, bool> MultiGraspGoalSet::getGoalId(unsigned int node_id, unsigned int grasp_id)
{
    auto iter = _roadmap_id_to_goal_id.find(node_id);
    if (iter == _roadmap_id_to_goal_id.end()) {
        return { 0, false };
    }
    // get the grasp for this goal
    bool valid_grasp = _goals.at(iter->second).grasp_id == grasp_id;
    return { iter->second, valid_grasp };
}

void MultiGraspGoalSet::getGoals(std::vector<MultiGraspMP::Goal>& goals) const
{
    goals.clear();
    for (auto elem : _goals) {
        goals.push_back(elem.second);
    }
}

/************************************** MGGoalDistance **************************************/

MGGoalDistance::MGGoalDistance(MultiGraspGoalSetConstPtr goal_set,
    const std::function<double(const Config&, const Config&)>& path_cost, double lambda)
{
    double max_q = -std::numeric_limits<double>::infinity();
    double min_q = std::numeric_limits<double>::infinity();
    // TODO just copying all goals like this is a bit inefficient
    std::vector<MultiGraspMP::Goal> goals;
    goal_set->getGoals(goals);
    // first compute min and max quality
    for (const MultiGraspMP::Goal& goal : goals) {
        // add goal to all goals
        max_q = std::max(max_q, goal.quality);
        min_q = std::min(min_q, goal.quality);
    }
    _quality_normalizer = (max_q - min_q);
    _quality_normalizer = _quality_normalizer == 0.0 ? 1.0 : _quality_normalizer;
    _goal_distance.scaled_lambda = lambda / _quality_normalizer;
    _goal_distance.path_cost = path_cost;
    _max_quality = max_q;
    // now add the goals to nearest neighbor data structures
    auto dist_fun = std::bind(&MGGoalDistance::GoalDistanceFn::distance, &_goal_distance, std::placeholders::_1, std::placeholders::_2);
    _all_goals.setDistanceFunction(dist_fun);
    for (auto& goal : goals) {
        _all_goals.add(goal);
        // add it to goals with the same grasp
        auto iter = _goals.find(goal.grasp_id);
        if (iter == _goals.end()) {
            // add new gnat for this grasp
            auto new_gnat = std::make_shared<::ompl::NearestNeighborsGNAT<MultiGraspMP::Goal>>();
            new_gnat->setDistanceFunction(dist_fun);
            new_gnat->add(goal);
            _goals.insert(std::make_pair(goal.grasp_id, new_gnat));
        } else {
            iter->second->add(goal);
        }
    }
}

MGGoalDistance::~MGGoalDistance() = default;

double MGGoalDistance::costToGo(const Config& a) const
{
    if (_all_goals.size() == 0) {
        throw std::logic_error("[MGGoalDistance::costToGo] No goals known. Can not compute cost to go.");
    }
    MultiGraspMP::Goal dummy_goal;
    dummy_goal.config = a;
    dummy_goal.quality = _max_quality;
    const auto& nn = _all_goals.nearest(dummy_goal);
    return _goal_distance.distance_const(nn, dummy_goal);
}

double MGGoalDistance::costToGo(const Config& a, unsigned int grasp_id) const
{
    auto iter = _goals.find(grasp_id);
    if (iter == _goals.end()) {
        throw std::logic_error("[MGGoalDistance::costToGo] Could not find GNAT for the given grasp " + std::to_string(grasp_id));
    }
    if (iter->second->size() == 0) {
        throw std::logic_error("[MGGoalDistance::costToGo] No goal known for the given grasp " + std::to_string(grasp_id));
    }
    MultiGraspMP::Goal dummy_goal;
    dummy_goal.config = a;
    dummy_goal.quality = _max_quality;
    const auto& nn = iter->second->nearest(dummy_goal);
    return _goal_distance.distance_const(nn, dummy_goal);
}

double MGGoalDistance::getGoalCost(double quality) const
{
    return _goal_distance.scaled_lambda * (_max_quality - quality);
}