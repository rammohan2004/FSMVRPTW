#include "route_utils.h"

#include <algorithm>
#include <fstream>
#include <iostream>

using namespace std;

double calculate_route_distance(const VRP &vrp, const vector<node_t> &route) {
  double total_distance = 0.0;
  if (route.empty()) {
    return total_distance;
  }

  total_distance += vrp.get_dist(DEPOT, route[0]);
  for (size_t i = 1; i < route.size(); i++) {
    total_distance += vrp.get_dist(route[i - 1], route[i]);
  }
  total_distance += vrp.get_dist(route[route.size() - 1], DEPOT);

  return total_distance;
}

bool route_is_empty(const vector<node_t> &route) {
  for (auto v : route) {
    if (v != DEPOT) {
      return false;
    }
  }
  return true;
}

// FSMVRPTW objective: distance travelled plus the fixed cost of each vehicle
// used. A route's vehicle is the cheapest type able to carry its load, so the
// fixed cost follows from the load -- see VRP::F.
//
// Empty routes are skipped. Their distance is already zero, but F(0) would
// return the cheapest type's fixed cost and charge for a vehicle that is not
// used.
double calculate_total_cost(const VRP &vrp,
                            const vector<vector<node_t>> &routes) {
  double total_cost = 0.0;
  for (const auto &route : routes) {
    if (route_is_empty(route)) {
      continue;
    }
    total_cost += calculate_route_distance(vrp, route);
    total_cost += vrp.F(vrp.get_route_load(route));
  }
  return total_cost;
}

double total_routing_cost(const VRP &vrp,
                          const vector<vector<node_t>> &routes) {
  double total = 0.0;
  for (const auto &route : routes) {
    if (route_is_empty(route)) {
      continue;
    }
    total += calculate_route_distance(vrp, route);
  }
  return total;
}

double total_fixed_cost(const VRP &vrp,
                        const vector<vector<node_t>> &routes) {
  double total = 0.0;
  for (const auto &route : routes) {
    if (route_is_empty(route)) {
      continue;
    }
    total += vrp.F(vrp.get_route_load(route));
  }
  return total;
}

int count_vehicles_used(const vector<vector<node_t>> &routes) {
  int used = 0;
  for (const auto &route : routes) {
    if (!route_is_empty(route)) {
      ++used;
    }
  }
  return used;
}

vector<int> fleet_mix(const VRP &vrp, const vector<vector<node_t>> &routes) {
  vector<int> per_type(vrp.numTypes(), 0);
  for (const auto &route : routes) {
    if (route_is_empty(route)) {
      continue;
    }
    int k = vrp.typeFor(vrp.get_route_load(route));
    if (k >= 0) {
      per_type[k]++;
    }
  }
  return per_type;
}

string mix_string(const VRP &vrp, const vector<vector<node_t>> &routes) {
  vector<int> per_type = fleet_mix(vrp, routes);
  string s;
  for (size_t k = 0; k < per_type.size(); ++k) {
    if (per_type[k] > 0) {
      s += vrp.types()[k].letter;
      s += to_string(per_type[k]);
    }
  }
  return s;
}

// Deliberately a different formulation from calculate_route_distance: this sums
// consecutive pairs only, adding no depot terms. The two agree exactly when
// routes are properly depot-wrapped, so a disagreement points at either a
// costing bug or a malformed route.
double recompute_routing_cost(const VRP &vrp,
                              const vector<vector<node_t>> &routes) {
  double total = 0.0;
  for (const auto &route : routes) {
    if (route_is_empty(route)) {
      continue;
    }
    for (size_t i = 1; i < route.size(); ++i) {
      total += vrp.get_dist(route[i - 1], route[i]);
    }
  }
  return total;
}

bool all_customers_visited_once(const VRP &vrp,
                                const vector<vector<node_t>> &routes) {
  vector<int> seen(vrp.getSize(), 0);
  for (const auto &route : routes) {
    for (auto v : route) {
      if (v != DEPOT) {
        seen[v]++;
      }
    }
  }
  for (size_t i = 1; i < vrp.getSize(); ++i) {
    if (seen[i] != 1) {
      return false;
    }
  }
  return true;
}

bool verify_route(const VRP &vrp, const vector<vector<node_t>> &routes) {
  demand_t vCapacity = vrp.maxCapacity();
  for (const auto &route : routes) {
    demand_t residueCap = vCapacity;
    tw_t process_time = 0;
    node_t prev = 0;
    for (auto v : route) {
      process_time += vrp.get_dist(prev, v);
      if (residueCap - vrp.node[v].demand >= 0 &&
          process_time <= vrp.node[v].latestTime) {
        residueCap = residueCap - vrp.node[v].demand;
        process_time =
            max(process_time, vrp.node[v].earlyTime) + vrp.node[v].serviceTime;
        prev = v;
      } else {
        return false;
      }
    }
  }
  return true;
}

bool verify_single_route(const VRP &vrp, const vector<node_t> &route) {
  demand_t vCapacity = vrp.maxCapacity();
  demand_t residueCap = vCapacity;
  tw_t process_time = 0;
  node_t prev = 0;
  for (auto v : route) {
    process_time += vrp.get_dist(prev, v);
    if (residueCap - vrp.node[v].demand >= 0 &&
        process_time <= vrp.node[v].latestTime) {
      residueCap = residueCap - vrp.node[v].demand;
      process_time =
          max(process_time, vrp.node[v].earlyTime) + vrp.node[v].serviceTime;
      prev = v;
    } else {
      return false;
    }
  }
  return true;
}

bool verify_tour_t(const VRP &vrp, const vector<node_t> &tour, node_t ncities) {
  tw_t process_time = 0;
  for (int i = 1; i < ncities; i++) {
    process_time += vrp.get_dist(tour[i - 1], tour[i]);
    if (process_time > vrp.node[tour[i]].latestTime) {
      return false;
    }
    process_time =
        max(process_time, vrp.node[tour[i]].earlyTime) + vrp.node[tour[i]].serviceTime;
  }
  return true;
}

bool verify_route_t(const VRP &vrp, const vector<vector<node_t>> &routes) {
  demand_t vCapacity = vrp.maxCapacity();
  for (const auto &route : routes) {
    demand_t residueCap = vCapacity;
    tw_t process_time = 0;
    node_t prev = 0;
    for (auto v : route) {
      process_time += vrp.get_dist(prev, v);
      if (residueCap - vrp.node[v].demand >= 0 &&
          process_time <= vrp.node[v].latestTime) {
        residueCap = residueCap - vrp.node[v].demand;
        process_time =
            max(process_time, vrp.node[v].earlyTime) + vrp.node[v].serviceTime;
        prev = v;
      } else {
        return false;
      }
    }
  }
  return true;
}

double calculate_tour_distance_t(const VRP &vrp,
                                 const vector<node_t> &tour,
                                 node_t ncities) {
  double total_distance = 0.0;
  for (int i = 1; i < ncities; i++) {
    total_distance += vrp.get_dist(tour[i - 1], tour[i]);
  }
  total_distance += vrp.get_dist(tour[ncities - 1], tour[0]);
  return total_distance;
}

double compute_waiting_time(const VRP &vrp, const vector<node_t> &route) {
  double time = 0.0;
  double total_wait = 0.0;
  node_t prev = 0;

  for (node_t node : route) {
    time += vrp.get_dist(prev, node);
    if (time < vrp.node[node].earlyTime) {
      total_wait += vrp.node[node].earlyTime - time;
      time = vrp.node[node].earlyTime;
    }
    time += vrp.node[node].serviceTime;
    prev = node;
  }

  return total_wait;
}

void print_routes(const vector<vector<node_t>> &routes) {
  cout << "Final Routes:" << endl;
  for (size_t i = 0; i < routes.size(); ++i) {
    cout << "Route #" << i + 1 << ": ";
    for (size_t j = 0; j < routes[i].size(); ++j) {
      cout << routes[i][j] << " ";
    }
    cout << endl;
  }
}

void save_routes_snapshot(const vector<vector<node_t>> &routes,
                          const string &filename) {
  ofstream file(filename);
  for (const auto &route : routes) {
    if (route.empty()) {
      continue;
    }

    file << "0 ";
    for (auto node : route) {
      file << node << " ";
    }
    file << "0\n";
  }
}

int max_length_of_route(const vector<vector<node_t>> &routes) {
  size_t max_length = 0;
  for (const auto &route : routes) {
    if (route.size() > max_length) {
      max_length = route.size();
    }
  }
  return static_cast<int>(max_length);
}
