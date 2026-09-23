#ifndef VRP_H
#define VRP_H

#include <cstddef>
#include <string>
#include <vector>

using point_t = double;
using weight_t = double;
using demand_t = double;
using node_t = int;
using tw_t = unsigned int;

const node_t DEPOT = 0;

class Point {
 public:
  point_t x;
  point_t y;
  demand_t demand;
  tw_t earlyTime;
  tw_t latestTime;
  tw_t serviceTime;

  Point();
};

// One vehicle type in the heterogeneous fleet. FSMVRPTW gives each type its own
// capacity and fixed cost, with an unlimited supply of each -- `count` is only
// the large stand-in number written in the instance file.
class VehicleType {
 public:
  char letter;         // A, B, C ... used in the fleet-mix string
  int count;           // vehicles available of this type
  demand_t capacity;
  double fixedCost;

  VehicleType();
  VehicleType(char l, int n, demand_t cap, double fc);
};

class VRP {
  size_t size;

  // Sorted by capacity ascending.
  std::vector<VehicleType> vtype;

  // Load -> vehicle lookup, built once by buildTypeTable() after parsing.
  // Read on every move evaluation, and maxCapInt_ is small (270 for the Braysy
  // tables), so the whole table sits in L1.
  int *typeTable;      // typeTable[L]  = cheapest type index carrying load L
  double *fixedTable;  // fixedTable[L] = that type's fixed cost
  int maxCapInt;       // largest integer load the tables cover

  void buildTypeTable();

 public:
  VRP();
  ~VRP();

  // VRP owns raw arrays and has a destructor, so a default copy would
  // double-free. It is only ever passed by reference, so copying is banned
  // outright rather than given a deep copy nobody needs.
  VRP(const VRP &) = delete;
  VRP &operator=(const VRP &) = delete;

  unsigned read(const std::string &filename);
  void print();
  void print_dist();

  // Fills dist with the pairwise distances. Replaces the old cal_graph_dist(),
  // which also built a full adjacency list -- 1.6 GB at 10,000 customers --
  // whose return value the caller discarded.
  void cal_dist();

  weight_t get_dist(node_t i, node_t j) const;
  size_t getSize() const;
  demand_t get_route_load(const std::vector<node_t> &route) const;

  // ---- fleet ----------------------------------------------------------
  const std::vector<VehicleType> &types() const;
  size_t numTypes() const;

  // Largest capacity in the fleet. A route is feasible on capacity grounds if
  // its load fits this, because some vehicle can then carry it.
  demand_t maxCapacity() const;

  // Index into types() of the CHEAPEST vehicle able to carry `load`, or -1 if
  // no type can. Cheapest rather than smallest: the two coincide when fixed
  // cost rises with capacity (Liu & Shen) but not in general, and the Braysy
  // et al. (2009) tables we use have cost per unit capacity falling.
  int typeFor(demand_t load) const {
    int L = static_cast<int>(load + 0.5);
    if (L < 0) L = 0;
    if (L > maxCapInt) return -1;
    return typeTable[L];
  }

  // Fixed cost of that vehicle -- Golden et al.'s F(z), used throughout the
  // FSMVRPTW literature. Returns +infinity when no vehicle can carry the load,
  // so an infeasible route can never look attractive to a cost comparison.
  double F(demand_t load) const {
    int L = static_cast<int>(load + 0.5);
    if (L < 0) L = 0;
    if (L > maxCapInt) return kInfeasible;
    return fixedTable[L];
  }

  static const double kInfeasible;

  // Flat arrays: 86 call sites index these as node[i] / dist[k], which reads
  // identically whether they are vectors or raw arrays, and a raw pointer can
  // be handed straight to a GPU later.
  Point *node;
  weight_t *dist;
};

#endif
