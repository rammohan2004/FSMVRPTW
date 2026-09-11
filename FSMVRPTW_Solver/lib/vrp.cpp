#include "vrp.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>

using namespace std;

Edge::Edge() = default;

Edge::Edge(node_t t, weight_t l) : to(t), length(l) {}

bool Edge::operator<(const Edge &e) { return length < e.length; }

Point::Point() = default;

VRP::VRP() : size(0), capacity(0) {}

VRP::~VRP() = default;

weight_t VRP::get_dist(node_t i, node_t j) const {
  if (i == j) {
    return 0.0;
  }

  if (i > j) {
    node_t temp = i;
    i = j;
    j = temp;
  }

  size_t myoffset = ((2 * i * size) - (i * i) + i) / 2;
  size_t correction = 2 * i + 1;
  return dist[myoffset + j - correction];
}

size_t VRP::getSize() const { return size; }

demand_t VRP::getCapacity() const { return capacity; }

demand_t VRP::get_route_load(const vector<node_t> &route) const {
  demand_t load = 0.0;
  for (auto current_node : route) {
    load += node[current_node].demand;
  }
  return load;
}

vector<vector<Edge>> VRP::cal_graph_dist() {
  dist.resize((size * (size - 1)) / 2);
  vector<vector<Edge>> nG(size);

  size_t k = 0;
  for (size_t i = 0; i < size; ++i) {
    for (size_t j = i + 1; j < size; ++j) {
      weight_t w =
          sqrt(((node[i].x - node[j].x) * (node[i].x - node[j].x)) +
               ((node[i].y - node[j].y) * (node[i].y - node[j].y)));

      dist[k] = w;
      nG[i].push_back(Edge(j, w));
      nG[j].push_back(Edge(i, w));
      ++k;
    }
  }

  return nG;
}

void VRP::print_dist() {
  for (size_t i = 0; i < size; ++i) {
    cout << i << ":";
    for (size_t j = 0; j < size; ++j) {
      cout << setw(10) << get_dist(i, j) << ' ';
    }
    cout << endl;
  }
}

unsigned VRP::read(const string &filename) {
  ifstream in(filename);
  if (!in.is_open()) {
    cerr << "Could not open the file \"" << filename << "\"" << endl;
    exit(1);
  }

  node.clear();
  dist.clear();

  string line;
  getline(in, line);
  string file_name = line;
  cout << "filename: " << file_name << endl;

  getline(in, line);
  getline(in, line);
  getline(in, line);

  int numVehicles;
  in >> numVehicles >> capacity;
  cout << "Vehicles: " << numVehicles << ", Capacity: " << capacity << endl;

  getline(in, line);
  getline(in, line);
  getline(in, line);
  getline(in, line);

  int id;
  double x, y, customer_demand, ready, due, service;
  while (in >> id >> x >> y >> customer_demand >> ready >> due >> service) {
    Point p;
    p.x = x;
    p.y = y;
    p.demand = customer_demand;
    p.earlyTime = ready;
    p.latestTime = due;
    p.serviceTime = service;
    node.push_back(p);
  }

  size = node.size();
  cout << "Total customers : " << size << endl;
  in.close();
  return capacity;
}

void VRP::print() {
  cout << "DIMENSION:" << size << '\n';
  cout << "CAPACITY:" << capacity << '\n';
  for (size_t i = 0; i < size; ++i) {
    cout << i << ':' << setw(6) << node[i].x << ' ' << setw(6) << node[i].y
         << ' ' << setw(6) << node[i].demand << endl;
  }
}
