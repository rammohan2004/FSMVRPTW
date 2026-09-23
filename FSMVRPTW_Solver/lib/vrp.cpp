#include "vrp.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

using namespace std;

Point::Point() = default;

VehicleType::VehicleType() : letter('A'), count(0), capacity(0), fixedCost(0) {}

VehicleType::VehicleType(char l, int n, demand_t cap, double fc)
    : letter(l), count(n), capacity(cap), fixedCost(fc) {}

const double VRP::kInfeasible = numeric_limits<double>::infinity();

VRP::VRP()
    : size(0),
      typeTable(nullptr),
      fixedTable(nullptr),
      maxCapInt(-1),
      node(nullptr),
      dist(nullptr) {}

VRP::~VRP() {
  delete[] typeTable;
  delete[] fixedTable;
  delete[] node;
  delete[] dist;
}

// Precompute, for every integer load up to the largest vehicle capacity, which
// vehicle type is cheapest for it. Called once after the fleet is parsed.
// Without this, typeFor() would scan the type list on every move evaluation.
void VRP::buildTypeTable() {
  delete[] typeTable;
  delete[] fixedTable;
  typeTable = nullptr;
  fixedTable = nullptr;

  maxCapInt = static_cast<int>(maxCapacity() + 0.5);
  if (maxCapInt < 0) {
    maxCapInt = 0;
  }

  const int num_types = static_cast<int>(vtype.size());
  const int n = maxCapInt + 1;
  typeTable = new int[n];
  fixedTable = new double[n];

  for (int L = 0; L < n; ++L) {
    int best = -1;
    double bestCost = 0.0;
    for (int k = 0; k < num_types; ++k) {
      if (vtype[k].capacity + 1e-9 >= L) {
        if (best < 0 || vtype[k].fixedCost < bestCost) {
          best = k;
          bestCost = vtype[k].fixedCost;
        }
      }
    }
    typeTable[L] = best;
    fixedTable[L] = (best < 0) ? kInfeasible : bestCost;
  }
}

weight_t VRP::get_dist(node_t i, node_t j) const {
  if (i == j) {
    return 0.0;
  }

  if (i > j) {
    node_t temp = i;
    i = j;
    j = temp;
  }

  // Promote to size_t BEFORE the arithmetic. node_t is a 32-bit int, so the
  // i*i term overflows for i >= 46341 (sqrt(INT_MAX)) and the offset becomes
  // garbage -- a segfault at 100,000 customers, while 10,000 (i*i = 1e8) is
  // comfortably inside range and looked fine.
  const size_t I = static_cast<size_t>(i);
  const size_t J = static_cast<size_t>(j);
  const size_t myoffset = ((2 * I * size) - (I * I) + I) / 2;
  const size_t correction = 2 * I + 1;
  return dist[myoffset + J - correction];
}

size_t VRP::getSize() const { return size; }

const vector<VehicleType> &VRP::types() const { return vtype; }

size_t VRP::numTypes() const { return vtype.size(); }

demand_t VRP::maxCapacity() const {
  demand_t m = 0;
  for (const auto &t : vtype) {
    if (t.capacity > m) {
      m = t.capacity;
    }
  }
  return m;
}

demand_t VRP::get_route_load(const vector<node_t> &route) const {
  demand_t load = 0.0;
  for (auto current_node : route) {
    load += node[current_node].demand;
  }
  return load;
}

void VRP::cal_dist() {
  delete[] dist;
  dist = new weight_t[(size * (size - 1)) / 2];

  size_t k = 0;
  for (size_t i = 0; i < size; ++i) {
    for (size_t j = i + 1; j < size; ++j) {
      weight_t dx = node[i].x - node[j].x;
      weight_t dy = node[i].y - node[j].y;
      dist[k] = sqrt(dx * dx + dy * dy);
      ++k;
    }
  }
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

// Reads our FSMVRPTW format:
//
//   <instance name>
//
//   VEHICLE
//   TYPE  NUMBER  CAPACITY  FIXED_COST
//      A    1000        40         200
//      ...                                   one row per vehicle type
//
//   CUSTOMER
//   CUST NO.  XCOORD.  YCOORD.  DEMAND  READY TIME  DUE DATE  SERVICE TIME
//      0  ...
//
// Section keywords are located rather than counted on, because the vehicle
// block is a variable number of rows.
unsigned VRP::read(const string &filename) {
  ifstream in(filename);
  if (!in.is_open()) {
    cerr << "Could not open the file \"" << filename << "\"" << endl;
    exit(1);
  }

  vector<Point> customers;
  vtype.clear();

  string line;
  getline(in, line);
  cout << "filename: " << line << endl;

  const int NONE = 0, VEHICLES = 1, CUSTOMERS = 2;
  int section = NONE;
  bool header_seen = false;

  while (getline(in, line)) {
    istringstream ss(line);
    string first;
    if (!(ss >> first)) {
      continue;  // blank line
    }

    if (first == "VEHICLE") {
      section = VEHICLES;
      header_seen = false;
      continue;
    }
    if (first == "CUSTOMER") {
      section = CUSTOMERS;
      header_seen = false;
      continue;
    }
    if (!header_seen) {
      header_seen = true;  // the column-title row of the current section
      continue;
    }

    if (section == VEHICLES) {
      int count;
      double cap, fixed_cost;
      if (ss >> count >> cap >> fixed_cost) {
        vtype.push_back(VehicleType(first[0], count, cap, fixed_cost));
      }
    } else if (section == CUSTOMERS) {
      double x, y, customer_demand, ready, due, service;
      if (ss >> x >> y >> customer_demand >> ready >> due >> service) {
        Point p;
        p.x = x;
        p.y = y;
        p.demand = customer_demand;
        p.earlyTime = ready;
        p.latestTime = due;
        p.serviceTime = service;
        customers.push_back(p);
      }
    }
  }
  in.close();

  if (vtype.empty()) {
    cerr << "No vehicle types found in \"" << filename << "\"" << endl;
    exit(1);
  }

  // Keep the fleet ordered by capacity so the listing and the mix string read
  // naturally.
  sort(vtype.begin(), vtype.end(),
       [](const VehicleType &a, const VehicleType &b) {
         return a.capacity < b.capacity;
       });

  buildTypeTable();

  size = customers.size();
  delete[] node;
  node = new Point[size];
  for (size_t i = 0; i < size; ++i) {
    node[i] = customers[i];
  }

  if (size < 2) {
    cerr << "Parsed fewer than 2 locations from \"" << filename << "\"" << endl;
    exit(1);
  }

  cout << "Vehicle types: " << vtype.size() << endl;
  for (const auto &t : vtype) {
    ostringstream ratio;
    ratio << fixed << setprecision(2)
          << (t.capacity > 0 ? t.fixedCost / t.capacity : 0.0);
    cout << "  " << t.letter << "  count " << setw(7) << t.count << "  capacity "
         << setw(7) << t.capacity << "  fixed cost " << setw(9) << t.fixedCost
         << "  cost/cap " << ratio.str() << endl;
  }
  cout << "Max capacity    : " << maxCapacity() << endl;
  cout << "Locations       : " << size << " (depot + " << size - 1
       << " customers)" << endl;

  return maxCapacity();
}

void VRP::print() {
  cout << "DIMENSION:" << size << '\n';
  cout << "VEHICLE TYPES:" << vtype.size() << '\n';
  for (const auto &t : vtype) {
    cout << "  " << t.letter << ' ' << t.capacity << ' ' << t.fixedCost << '\n';
  }
  for (size_t i = 0; i < size; ++i) {
    cout << i << ':' << setw(6) << node[i].x << ' ' << setw(6) << node[i].y
         << ' ' << setw(6) << node[i].demand << endl;
  }
}
