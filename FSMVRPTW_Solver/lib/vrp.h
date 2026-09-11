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

class Edge {
 public:
  node_t to;
  weight_t length;

  Edge();
  Edge(node_t t, weight_t l);
  bool operator<(const Edge &e);
};

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

class VRP {
  size_t size;
  demand_t capacity;
  std::string type;

 public:
  VRP();
  ~VRP();

  unsigned read(const std::string &filename);
  void print();
  void print_dist();
  std::vector<std::vector<Edge>> cal_graph_dist();

  weight_t get_dist(node_t i, node_t j) const;
  size_t getSize() const;
  demand_t getCapacity() const;
  demand_t get_route_load(const std::vector<node_t> &route) const;

  std::vector<Point> node;
  std::vector<weight_t> dist;
};

#endif
