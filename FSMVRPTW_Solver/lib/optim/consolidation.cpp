#include "consolidation.h"

#include <algorithm>
#include <numeric>
#include <utility>

#include "../route_utils.h"

using namespace std;

// K nearest customers of each customer, by distance. Built once per call.
//
// This is what keeps the move affordable. Without it, finding where to reinsert
// a customer means scanning every position of every route -- O(n) positions per
// customer, and the whole move becomes quadratic in the customer count. A
// customer is only ever worth inserting next to one of its close neighbours, so
// restricting anchors to the K nearest costs almost nothing in quality.
vector<vector<node_t>> build_neighbour_lists(const VRP &vrp, int K) {
  const size_t n = vrp.getSize();
  vector<vector<node_t>> knn(n);
  if (n < 2) {
    return knn;
  }

  vector<pair<weight_t, node_t>> candidates;
  candidates.reserve(n);

  for (size_t i = 1; i < n; ++i) {
    candidates.clear();
    for (size_t j = 1; j < n; ++j) {
      if (i == j) continue;
      candidates.push_back(
          {vrp.get_dist(static_cast<node_t>(i), static_cast<node_t>(j)),
           static_cast<node_t>(j)});
    }
    const size_t k = min(static_cast<size_t>(K), candidates.size());
    partial_sort(candidates.begin(), candidates.begin() + k, candidates.end());
    knn[i].reserve(k);
    for (size_t t = 0; t < k; ++t) {
      knn[i].push_back(candidates[t].second);
    }
  }
  return knn;
}

namespace {

int customer_count(const vector<node_t> &route) {
  int c = 0;
  for (node_t v : route) {
    if (v != DEPOT) ++c;
  }
  return c;
}

// Where inserting `c` into `route` at `pos` would land, as a cost delta.
// Distance only -- the caller adds the receiving route's fixed-cost step.
double insertion_distance_delta(const VRP &vrp, const vector<node_t> &route,
                                size_t pos, node_t c) {
  const node_t prev = route[pos - 1];
  const node_t next = route[pos];
  return vrp.get_dist(prev, c) + vrp.get_dist(c, next) -
         vrp.get_dist(prev, next);
}

}  // namespace

bool consolidate_routes(const VRP &vrp, vector<vector<node_t>> &routes,
                        const vector<vector<node_t>> &knn) {
  if (routes.size() < 2) {
    return false;
  }

  const demand_t cap = vrp.maxCapacity();
  bool eliminated_any = false;

  bool progress = true;
  while (progress) {
    progress = false;

    // Which route each customer currently sits in, and what each route carries.
    vector<int> route_of(vrp.getSize(), -1);
    vector<double> load(routes.size(), 0.0);
    for (size_t r = 0; r < routes.size(); ++r) {
      for (node_t v : routes[r]) {
        if (v != DEPOT) route_of[v] = static_cast<int>(r);
      }
      load[r] = vrp.get_route_load(routes[r]);
    }

    // Try the routes with fewest customers first: they are the cheapest to empty
    // and the most likely to succeed, so a pass finds eliminations early.
    vector<int> order(routes.size());
    iota(order.begin(), order.end(), 0);
    sort(order.begin(), order.end(), [&](int a, int b) {
      return customer_count(routes[a]) < customer_count(routes[b]);
    });

    for (int victim : order) {
      if (route_is_empty(routes[victim])) continue;

      vector<node_t> displaced;
      for (node_t v : routes[victim]) {
        if (v != DEPOT) displaced.push_back(v);
      }
      if (displaced.empty()) continue;

      // What eliminating this route is worth: its fixed cost plus all its travel.
      const double gain = vrp.F(load[victim]) +
                          calculate_route_distance(vrp, routes[victim]);

      // Work on a copy so a failed attempt costs nothing. All-or-nothing: if any
      // single customer cannot be placed, the route is not eliminated and the
      // original stands.
      vector<vector<node_t>> work = routes;
      vector<double> work_load = load;
      work[victim] = vector<node_t>{DEPOT, DEPOT};
      work_load[victim] = 0.0;

      double spend = 0.0;
      bool all_placed = true;

      for (node_t c : displaced) {
        double best_delta = 0.0;
        int best_route = -1;
        size_t best_pos = 0;
        bool found = false;

        for (node_t anchor : knn[c]) {
          const int r = route_of[anchor];
          // Skip customers from the route being emptied: they have no settled
          // position to anchor against.
          if (r < 0 || r == victim) continue;
          if (work_load[r] + vrp.node[c].demand > cap) continue;

          // The receiving route may need a larger, dearer vehicle once c is on
          // board. Charging that here is what stops consolidation from quietly
          // trading one cheap vehicle for one expensive one.
          const double fixed_delta = vrp.F(work_load[r] + vrp.node[c].demand) -
                                     vrp.F(work_load[r]);

          for (size_t p = 1; p < work[r].size(); ++p) {
            if (work[r][p - 1] != anchor && work[r][p] != anchor) continue;

            const double delta =
                insertion_distance_delta(vrp, work[r], p, c) + fixed_delta;
            if (found && delta >= best_delta) continue;

            // Feasibility last: it is the expensive check. Insert, test, undo.
            work[r].insert(work[r].begin() + p, c);
            const bool feasible = verify_single_route(vrp, work[r]);
            work[r].erase(work[r].begin() + p);
            if (!feasible) continue;

            best_delta = delta;
            best_route = r;
            best_pos = p;
            found = true;
          }
        }

        if (!found) {
          all_placed = false;
          break;
        }

        work[best_route].insert(work[best_route].begin() + best_pos, c);
        work_load[best_route] += vrp.node[c].demand;
        spend += best_delta;
      }

      if (!all_placed || spend >= gain) continue;

      routes = std::move(work);
      eliminated_any = true;
      progress = true;
      break;  // loads and route_of are now stale; restart the pass
    }
  }

  // Drop the emptied shells so the fleet count reflects reality.
  routes.erase(remove_if(routes.begin(), routes.end(),
                         [](const vector<node_t> &r) {
                           return route_is_empty(r);
                         }),
               routes.end());

  return eliminated_any;
}
