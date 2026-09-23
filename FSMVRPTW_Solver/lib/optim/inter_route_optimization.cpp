#include "inter_route_optimization.h"
#include<iostream>
#include <vector>
#include <omp.h>
#include "../route_utils.h"

using namespace std;

namespace {

// Fixed cost of a route carrying `load` with `ncust` customers.
//
// The ncust guard matters: F(0) returns the CHEAPEST vehicle's fixed cost, not
// zero, so a route emptied by a move would still be charged for a vehicle and
// the move evaluation would never see the saving that emptying it produces.
inline double fixed_for(const VRP &vrp, demand_t load, int ncust) {
  return ncust > 0 ? vrp.F(load) : 0.0;
}

}  // namespace

void inter_route_relocate(const VRP &vrp, vector<vector<node_t>> &routes) {
  cout<<"Starting sequential inter-route relocate optimization..."<<endl;
  bool improvement = true;

  while (improvement) {
    improvement = false;
    double global_best_gain = 1e-6;
    int best_r1 = -1;
    int best_r2 = -1;
    vector<node_t> best_routeA;
    vector<node_t> best_routeB;

    const int num_routes = static_cast<int>(routes.size());

    for (int r1 = 0; r1 < num_routes; r1++) {
      for (int r2 = 0; r2 < num_routes; r2++) {
        if (r1 == r2) continue;

        const auto &routeA = routes[r1];
        const auto &routeB = routes[r2];
        if (routeA.size() <= 2) continue;

        // FLEET-AWARE. Relocating a customer changes both routes' loads, which
        // can push either across a vehicle-capacity threshold and change which
        // vehicle it needs. Scored on distance alone the move happily traded a
        // cheap vehicle for a dearer one; on these instances fixed cost is ~64%
        // of the objective, so that is the majority of what is at stake.
        // Hoisted out of the i/j loops: it depends only on the route pair.
        const demand_t loadA = vrp.get_route_load(routeA);
        const demand_t loadB = vrp.get_route_load(routeB);
        const int nA = static_cast<int>(routeA.size()) - 2;
        const int nB = static_cast<int>(routeB.size()) - 2;
        const double fixedA_before = fixed_for(vrp, loadA, nA);
        const double fixedB_before = fixed_for(vrp, loadB, nB);

        for (size_t i = 1; i < routeA.size() - 1; i++) {
          node_t u = routeA[i];
          node_t t = routeA[i - 1];
          node_t w = routeA[i + 1];

          const demand_t du = vrp.node[u].demand;
          // Route A may empty entirely here -- that is the big win, and the
          // ncust guard in fixed_for is what lets the move see it.
          const double fixed_delta =
              (fixed_for(vrp, loadA - du, nA - 1) - fixedA_before) +
              (fixed_for(vrp, loadB + du, nB + 1) - fixedB_before);

          double savings_A =
              vrp.get_dist(t, u) + vrp.get_dist(u, w) - vrp.get_dist(t, w);

          for (size_t j = 1; j < routeB.size(); j++) {
            node_t x = routeB[j - 1];
            node_t y = routeB[j];

            double cost_B =
                vrp.get_dist(x, u) + vrp.get_dist(u, y) - vrp.get_dist(x, y);
            double total_gain = savings_A - cost_B - fixed_delta;

            if (total_gain > global_best_gain) {
              vector<node_t> new_routeA = routeA;
              vector<node_t> new_routeB = routeB;

              new_routeA.erase(new_routeA.begin() + i);
              new_routeB.insert(new_routeB.begin() + j, u);

              if (verify_single_route(vrp, new_routeA) &&
                  verify_single_route(vrp, new_routeB)) {
                global_best_gain = total_gain;
                best_r1 = r1;
                best_r2 = r2;
                best_routeA = std::move(new_routeA);
                best_routeB = std::move(new_routeB);
              }
            }
          }
        }
      }
    }

    if (global_best_gain > 1e-6) {
      routes[best_r1] = std::move(best_routeA);
      routes[best_r2] = std::move(best_routeB);
      improvement = true;
    }

    for (auto it = routes.begin(); it != routes.end();) {
      if (it->size() <= 2) {
        it = routes.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void inter_route_relocate_parallel(const VRP &vrp, vector<vector<node_t>> &routes) {
  bool improvement = true;

  while (improvement) {
    improvement = false;

    double global_best_gain = 1e-6; 
    int best_r1 = -1;
    int best_r2 = -1;
    vector<node_t> best_routeA;
    vector<node_t> best_routeB;

    int num_routes = routes.size();

    // --- PARALLEL SEARCH REGION ---
    #pragma omp parallel
    {
      double local_best_gain = 1e-6;
      int local_best_r1 = -1;
      int local_best_r2 = -1;
      vector<node_t> local_best_routeA;
      vector<node_t> local_best_routeB;

      #pragma omp for schedule(dynamic) collapse(2) nowait
      for (int r1 = 0; r1 < num_routes; r1++) {
        for (int r2 = 0; r2 < num_routes; r2++) {
          if (r1 == r2) continue;

          const auto &routeA = routes[r1];
          const auto &routeB = routes[r2];
          
          if (routeA.size() <= 2) continue;

          // Fleet-aware, as in the sequential version above.
          const demand_t loadA = vrp.get_route_load(routeA);
          const demand_t loadB = vrp.get_route_load(routeB);
          const int nA = static_cast<int>(routeA.size()) - 2;
          const int nB = static_cast<int>(routeB.size()) - 2;
          const double fixedA_before = fixed_for(vrp, loadA, nA);
          const double fixedB_before = fixed_for(vrp, loadB, nB);

          for (size_t i = 1; i < routeA.size() - 1; i++) {
            node_t u = routeA[i];
            node_t t = routeA[i - 1];
            node_t w = routeA[i + 1];

            const demand_t du = vrp.node[u].demand;
            const double fixed_delta =
                (fixed_for(vrp, loadA - du, nA - 1) - fixedA_before) +
                (fixed_for(vrp, loadB + du, nB + 1) - fixedB_before);

            double savings_A = vrp.get_dist(t, u) + vrp.get_dist(u, w) - vrp.get_dist(t, w);

            for (size_t j = 1; j < routeB.size(); j++) {
              node_t x = routeB[j - 1];
              node_t y = routeB[j];

              double cost_B = vrp.get_dist(x, u) + vrp.get_dist(u, y) - vrp.get_dist(x, y);
              double total_gain = savings_A - cost_B - fixed_delta;

              if (total_gain > local_best_gain) {
                
                vector<node_t> new_routeA = routeA;
                vector<node_t> new_routeB = routeB;

                new_routeA.erase(new_routeA.begin() + i);
                new_routeB.insert(new_routeB.begin() + j, u);

                if (verify_single_route(vrp, new_routeA) && verify_single_route(vrp, new_routeB)) {
                  local_best_gain = total_gain;
                  local_best_r1 = r1;
                  local_best_r2 = r2;
                  local_best_routeA = std::move(new_routeA);
                  local_best_routeB = std::move(new_routeB);
                }
              }
            }
          }
        }
      }

      #pragma omp critical
      {
        if (local_best_gain > global_best_gain) {
          global_best_gain = local_best_gain;
          best_r1 = local_best_r1;
          best_r2 = local_best_r2;
          best_routeA = std::move(local_best_routeA);
          best_routeB = std::move(local_best_routeB);
        }
      }
    } // --- END OF PARALLEL REGION ---

    if (global_best_gain > 1e-6) {
      routes[best_r1] = std::move(best_routeA);
      routes[best_r2] = std::move(best_routeB);
      improvement = true;
    }

    for (auto it = routes.begin(); it != routes.end();) {
      if (it->size() <= 2) {
        it = routes.erase(it);
      } else {
        ++it;
      }
    }
  }
}


void inter_route_swap(const VRP &vrp, vector<vector<node_t>> &routes) {
  cout<<"Starting sequential inter-route swap optimization..."<<endl;
  bool improvement = true;

  while (improvement) {
    improvement = false;

    double global_best_gain = 1e-6;
    int best_r1 = -1;
    int best_r2 = -1;
    vector<node_t> best_routeA;
    vector<node_t> best_routeB;

    const int num_routes = static_cast<int>(routes.size());

    for (int r1 = 0; r1 < num_routes; r1++) {
      for (int r2 = r1 + 1; r2 < num_routes; r2++) {
        const auto &routeA = routes[r1];
        const auto &routeB = routes[r2];

        if (routeA.size() <= 2 || routeB.size() <= 2) continue;

        double base_load_A = vrp.get_route_load(routeA);
        double base_load_B = vrp.get_route_load(routeB);

        for (size_t i = 1; i < routeA.size() - 1; i++) {
          node_t u = routeA[i];
          node_t t = routeA[i - 1];
          node_t w = routeA[i + 1];

          for (size_t j = 1; j < routeB.size() - 1; j++) {
            node_t v = routeB[j];
            node_t x = routeB[j - 1];
            node_t y = routeB[j + 1];

            double cost_before = vrp.get_dist(t, u) + vrp.get_dist(u, w) +
                                 vrp.get_dist(x, v) + vrp.get_dist(v, y);
            double cost_after = vrp.get_dist(t, v) + vrp.get_dist(v, w) +
                                vrp.get_dist(x, u) + vrp.get_dist(u, y);
            // Fleet-aware: a swap changes both loads, so it can change which
            // vehicle each route needs even though neither route changes length.
            const double swapped_A =
                base_load_A - vrp.node[u].demand + vrp.node[v].demand;
            const double swapped_B =
                base_load_B - vrp.node[v].demand + vrp.node[u].demand;
            // Neither route can empty (both are guarded size > 2), so the counts
            // are unchanged and any positive value serves as the ncust argument.
            const double swap_fixed_delta =
                (fixed_for(vrp, swapped_A, 1) + fixed_for(vrp, swapped_B, 1)) -
                (fixed_for(vrp, base_load_A, 1) + fixed_for(vrp, base_load_B, 1));

            double total_gain = cost_before - cost_after - swap_fixed_delta;

            if (total_gain > global_best_gain) {
              double new_load_A =
                  base_load_A - vrp.node[u].demand + vrp.node[v].demand;
              double new_load_B =
                  base_load_B - vrp.node[v].demand + vrp.node[u].demand;

              if (new_load_A <= vrp.maxCapacity() &&
                  new_load_B <= vrp.maxCapacity()) {
                vector<node_t> new_routeA = routeA;
                vector<node_t> new_routeB = routeB;

                new_routeA[i] = v;
                new_routeB[j] = u;

                if (verify_single_route(vrp, new_routeA) &&
                    verify_single_route(vrp, new_routeB)) {
                  global_best_gain = total_gain;
                  best_r1 = r1;
                  best_r2 = r2;
                  best_routeA = std::move(new_routeA);
                  best_routeB = std::move(new_routeB);
                }
              }
            }
          }
        }
      }
    }

    if (global_best_gain > 1e-6) {
      routes[best_r1] = std::move(best_routeA);
      routes[best_r2] = std::move(best_routeB);
      improvement = true;
    }
  }
}

void inter_route_swap_parallel(const VRP &vrp, vector<vector<node_t>> &routes) {
  bool improvement = true;

  while (improvement) {
    improvement = false;

    double global_best_gain = 1e-6; 
    int best_r1 = -1;
    int best_r2 = -1;
    vector<node_t> best_routeA;
    vector<node_t> best_routeB;

    int num_routes = routes.size();

    // --- PARALLEL SEARCH REGION ---
    #pragma omp parallel
    {
      double local_best_gain = 1e-6;
      int local_best_r1 = -1;
      int local_best_r2 = -1;
      vector<node_t> local_best_routeA;
      vector<node_t> local_best_routeB;

      #pragma omp for schedule(dynamic) nowait
      for (int r1 = 0; r1 < num_routes; r1++) {
        for (int r2 = r1 + 1; r2 < num_routes; r2++) {
          
          const auto &routeA = routes[r1];
          const auto &routeB = routes[r2];

          if (routeA.size() <= 2 || routeB.size() <= 2) {
            continue;
          }

          double base_load_A = vrp.get_route_load(routeA);
          double base_load_B = vrp.get_route_load(routeB);

          for (size_t i = 1; i < routeA.size() - 1; i++) {
            node_t u = routeA[i];
            node_t t = routeA[i - 1];
            node_t w = routeA[i + 1];

            for (size_t j = 1; j < routeB.size() - 1; j++) {
              node_t v = routeB[j];
              node_t x = routeB[j - 1];
              node_t y = routeB[j + 1];

              double cost_before = vrp.get_dist(t, u) + vrp.get_dist(u, w) +
                                   vrp.get_dist(x, v) + vrp.get_dist(v, y);
              double cost_after = vrp.get_dist(t, v) + vrp.get_dist(v, w) +
                                  vrp.get_dist(x, u) + vrp.get_dist(u, y);

              // Fleet-aware, as in the sequential swap.
              const double swapped_A =
                  base_load_A - vrp.node[u].demand + vrp.node[v].demand;
              const double swapped_B =
                  base_load_B - vrp.node[v].demand + vrp.node[u].demand;
              const double swap_fixed_delta =
                  (fixed_for(vrp, swapped_A, 1) + fixed_for(vrp, swapped_B, 1)) -
                  (fixed_for(vrp, base_load_A, 1) +
                   fixed_for(vrp, base_load_B, 1));

              double total_gain = cost_before - cost_after - swap_fixed_delta;

              if (total_gain > local_best_gain) {
                double new_load_A = base_load_A - vrp.node[u].demand + vrp.node[v].demand;
                double new_load_B = base_load_B - vrp.node[v].demand + vrp.node[u].demand;

                if (new_load_A <= vrp.maxCapacity() && new_load_B <= vrp.maxCapacity()) {
                  vector<node_t> new_routeA = routeA;
                  vector<node_t> new_routeB = routeB;

                  new_routeA[i] = v;
                  new_routeB[j] = u;
                  if (verify_single_route(vrp, new_routeA) &&
                      verify_single_route(vrp, new_routeB)) {
                    local_best_gain = total_gain;
                    local_best_r1 = r1;
                    local_best_r2 = r2;
                    local_best_routeA = std::move(new_routeA);
                    local_best_routeB = std::move(new_routeB);
                  }
                }
              }
            }
          }
        }
      }

      #pragma omp critical
      {
        if (local_best_gain > global_best_gain) {
          global_best_gain = local_best_gain;
          best_r1 = local_best_r1;
          best_r2 = local_best_r2;
          best_routeA = std::move(local_best_routeA);
          best_routeB = std::move(local_best_routeB);
        }
      }
    } // --- END OF PARALLEL REGION ---
    if (global_best_gain > 1e-6) {
      routes[best_r1] = std::move(best_routeA);
      routes[best_r2] = std::move(best_routeB);
      improvement = true;
    }
  }
}



void inter_route_2opt_star(const VRP &vrp, vector<vector<node_t>> &routes) {
  cout<<"Starting sequential inter-route 2-opt* optimization..."<<endl;
  bool improvement = true;

  while (improvement) {
    improvement = false;

    double global_best_gain = 1e-6;
    int best_r1 = -1;
    int best_r2 = -1;
    vector<node_t> best_routeA;
    vector<node_t> best_routeB;

    const int num_routes = static_cast<int>(routes.size());

    for (int r1 = 0; r1 < num_routes; r1++) {
      for (int r2 = r1 + 1; r2 < num_routes; r2++) {
        const auto &routeA = routes[r1];
        const auto &routeB = routes[r2];

        if (routeA.size() <= 2 || routeB.size() <= 2) continue;

        // Prefix loads, so a tail exchange's resulting loads are O(1) to score.
        vector<demand_t> prefA(routeA.size()), prefB(routeB.size());
        prefA[0] = vrp.node[routeA[0]].demand;
        for (size_t k = 1; k < routeA.size(); ++k)
          prefA[k] = prefA[k - 1] + vrp.node[routeA[k]].demand;
        prefB[0] = vrp.node[routeB[0]].demand;
        for (size_t k = 1; k < routeB.size(); ++k)
          prefB[k] = prefB[k - 1] + vrp.node[routeB[k]].demand;
        const demand_t loadA_total = prefA.back();
        const demand_t loadB_total = prefB.back();

        for (size_t i = 0; i < routeA.size() - 1; i++) {
          node_t t = routeA[i];
          node_t u = routeA[i + 1];

          for (size_t j = 0; j < routeB.size() - 1; j++) {
            node_t x = routeB[j];
            node_t v = routeB[j + 1];

            double cost_before = vrp.get_dist(t, u) + vrp.get_dist(x, v);
            double cost_after = vrp.get_dist(t, v) + vrp.get_dist(x, u);
            // Fleet-aware. 2-opt* exchanges route TAILS, so each new route is a
            // prefix of one and a suffix of the other; the loads follow from the
            // prefix sums hoisted above. Either route can end up empty, which is
            // exactly the case worth finding, so the customer counts are tracked
            // and fixed_for charges nothing for an empty route.
            const demand_t newA = prefA[i] + (loadB_total - prefB[j]);
            const demand_t newB = prefB[j] + (loadA_total - prefA[i]);
            const int lenA = static_cast<int>(routeA.size());
            const int lenB = static_cast<int>(routeB.size());
            const int ncA = static_cast<int>(i) + max(0, lenB - 2 - static_cast<int>(j));
            const int ncB = static_cast<int>(j) + max(0, lenA - 2 - static_cast<int>(i));
            const double opt_fixed_delta =
                (fixed_for(vrp, newA, ncA) + fixed_for(vrp, newB, ncB)) -
                (fixed_for(vrp, loadA_total, lenA - 2) +
                 fixed_for(vrp, loadB_total, lenB - 2));

            double total_gain = cost_before - cost_after - opt_fixed_delta;

            if (total_gain > global_best_gain) {
              vector<node_t> new_routeA;
              vector<node_t> new_routeB;

              new_routeA.reserve(routeA.size() + routeB.size());
              new_routeB.reserve(routeA.size() + routeB.size());

              new_routeA.insert(new_routeA.end(), routeA.begin(),
                                routeA.begin() + i + 1);
              new_routeA.insert(new_routeA.end(), routeB.begin() + j + 1,
                                routeB.end());

              new_routeB.insert(new_routeB.end(), routeB.begin(),
                                routeB.begin() + j + 1);
              new_routeB.insert(new_routeB.end(), routeA.begin() + i + 1,
                                routeA.end());

              double new_load_A = vrp.get_route_load(new_routeA);
              double new_load_B = vrp.get_route_load(new_routeB);

              if (new_load_A <= vrp.maxCapacity() &&
                  new_load_B <= vrp.maxCapacity()) {
                if (verify_single_route(vrp, new_routeA) &&
                    verify_single_route(vrp, new_routeB)) {
                  global_best_gain = total_gain;
                  best_r1 = r1;
                  best_r2 = r2;
                  best_routeA = std::move(new_routeA);
                  best_routeB = std::move(new_routeB);
                }
              }
            }
          }
        }
      }
    }

    if (global_best_gain > 1e-6) {
      routes[best_r1] = std::move(best_routeA);
      routes[best_r2] = std::move(best_routeB);
      improvement = true;
    }

    for (auto it = routes.begin(); it != routes.end();) {
      if (it->size() <= 2) {
        it = routes.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void inter_route_2opt_star_parallel(const VRP &vrp, vector<vector<node_t>> &routes) {
  bool improvement = true;

  while (improvement) {
    improvement = false;

    double global_best_gain = 1e-6;
    int best_r1 = -1;
    int best_r2 = -1;
    vector<node_t> best_routeA;
    vector<node_t> best_routeB;

    int num_routes = routes.size();

    // --- PARALLEL SEARCH REGION ---
    #pragma omp parallel
    {
      double local_best_gain = 1e-6;
      int local_best_r1 = -1;
      int local_best_r2 = -1;
      vector<node_t> local_best_routeA;
      vector<node_t> local_best_routeB;

      #pragma omp for schedule(dynamic) nowait
      for (int r1 = 0; r1 < num_routes; r1++) {
        for (int r2 = r1 + 1; r2 < num_routes; r2++) {
          
          const auto &routeA = routes[r1];
          const auto &routeB = routes[r2];

          if (routeA.size() <= 2 || routeB.size() <= 2) {
            continue;
          }

          // Prefix loads, as in the sequential version.
          vector<demand_t> prefA(routeA.size()), prefB(routeB.size());
          prefA[0] = vrp.node[routeA[0]].demand;
          for (size_t k = 1; k < routeA.size(); ++k)
            prefA[k] = prefA[k - 1] + vrp.node[routeA[k]].demand;
          prefB[0] = vrp.node[routeB[0]].demand;
          for (size_t k = 1; k < routeB.size(); ++k)
            prefB[k] = prefB[k - 1] + vrp.node[routeB[k]].demand;
          const demand_t loadA_total = prefA.back();
          const demand_t loadB_total = prefB.back();

          for (size_t i = 0; i < routeA.size() - 1; i++) {
            node_t t = routeA[i];
            node_t u = routeA[i + 1];

            for (size_t j = 0; j < routeB.size() - 1; j++) {
              node_t x = routeB[j];
              node_t v = routeB[j + 1];

              double cost_before = vrp.get_dist(t, u) + vrp.get_dist(x, v);
              double cost_after = vrp.get_dist(t, v) + vrp.get_dist(x, u);

              // Fleet-aware, as in the sequential 2-opt*.
              const demand_t newA = prefA[i] + (loadB_total - prefB[j]);
              const demand_t newB = prefB[j] + (loadA_total - prefA[i]);
              const int lenA = static_cast<int>(routeA.size());
              const int lenB = static_cast<int>(routeB.size());
              const int ncA =
                  static_cast<int>(i) + max(0, lenB - 2 - static_cast<int>(j));
              const int ncB =
                  static_cast<int>(j) + max(0, lenA - 2 - static_cast<int>(i));
              const double opt_fixed_delta =
                  (fixed_for(vrp, newA, ncA) + fixed_for(vrp, newB, ncB)) -
                  (fixed_for(vrp, loadA_total, lenA - 2) +
                   fixed_for(vrp, loadB_total, lenB - 2));

              double total_gain = cost_before - cost_after - opt_fixed_delta;

              if (total_gain > local_best_gain) {
                
                vector<node_t> new_routeA;
                vector<node_t> new_routeB;

                new_routeA.reserve(routeA.size() + routeB.size());
                new_routeB.reserve(routeA.size() + routeB.size());

                // Reconnect: Route A prefix + Route B suffix
                new_routeA.insert(new_routeA.end(), routeA.begin(), routeA.begin() + i + 1);
                new_routeA.insert(new_routeA.end(), routeB.begin() + j + 1, routeB.end());

                // Reconnect: Route B prefix + Route A suffix
                new_routeB.insert(new_routeB.end(), routeB.begin(), routeB.begin() + j + 1);
                new_routeB.insert(new_routeB.end(), routeA.begin() + i + 1, routeA.end());

                double new_load_A = vrp.get_route_load(new_routeA);
                double new_load_B = vrp.get_route_load(new_routeB);

                if (new_load_A <= vrp.maxCapacity() && new_load_B <= vrp.maxCapacity()) {
                  if (verify_single_route(vrp, new_routeA) && verify_single_route(vrp, new_routeB)) {
                    
                    local_best_gain = total_gain;
                    local_best_r1 = r1;
                    local_best_r2 = r2;
                    local_best_routeA = std::move(new_routeA);
                    local_best_routeB = std::move(new_routeB);
                  }
                }
              }
            }
          }
        }
      }

      #pragma omp critical
      {
        if (local_best_gain > global_best_gain) {
          global_best_gain = local_best_gain;
          best_r1 = local_best_r1;
          best_r2 = local_best_r2;
          best_routeA = std::move(local_best_routeA);
          best_routeB = std::move(local_best_routeB);
        }
      }
    } // --- END OF PARALLEL REGION ---

    if (global_best_gain > 1e-6) {
      routes[best_r1] = std::move(best_routeA);
      routes[best_r2] = std::move(best_routeB);
      improvement = true;
    }

    for (auto it = routes.begin(); it != routes.end();) {
      if (it->size() <= 2) {
        it = routes.erase(it);
      } else {
        ++it;
      }
    }
  }
}


// NOT CALLED from solve_cvrptw.cpp, and still FLEET-BLIND: its gain is pure
// distance, with no F(load) term, so it can trade a cheap vehicle for a dearer
// one and raise total cost while reporting an improvement. Give it the same
// treatment as inter_route_relocate above before enabling it.
void updated_relocate(const VRP &vrp, vector<vector<node_t>> &routes) {
  bool improvement = true;

  while (improvement) {
    improvement = false;

    for (size_t r1 = 0; r1 < routes.size(); r1++) {
      for (size_t r2 = 0; r2 < routes.size(); r2++) {
        if (r1 == r2) {
          continue;
        }

        auto &routeA = routes[r1];
        auto &routeB = routes[r2];

        if (routeA.size() <= 2) {
          continue;
        }

        for (size_t i = 1; i < routeA.size() - 1; i++) {
          node_t u = routeA[i];
          node_t t = routeA[i - 1];
          node_t w = routeA[i + 1];

          double savings_A =
              vrp.get_dist(t, u) + vrp.get_dist(u, w) - vrp.get_dist(t, w);

          vector<node_t> new_routeA = routeA;
          new_routeA.erase(new_routeA.begin() + i);
          if (!verify_single_route(vrp, new_routeA)) {
            continue;
          }

          vector<node_t> best_routeB;
          double best_gain = 0.0;

          for (size_t j = 1; j < routeB.size(); j++) {
            node_t x = routeB[j - 1];
            node_t y = routeB[j];

            double cost_B =
                vrp.get_dist(x, u) + vrp.get_dist(u, y) - vrp.get_dist(x, y);
            double total_gain = savings_A - cost_B;

            if (total_gain > 1e-6 && total_gain > best_gain) {
              vector<node_t> new_routeB = routeB;
              new_routeB.insert(new_routeB.begin() + j, u);
              if (verify_single_route(vrp, new_routeB)) {
                best_gain = total_gain;
                best_routeB = new_routeB;
              }
            }
          }

          if (best_gain > 1e-6) {
            routeA = new_routeA;
            routeB = best_routeB;
            improvement = true;
            goto end_of_search;
          }
        }
      }
    }
  end_of_search:
    for (auto it = routes.begin(); it != routes.end();) {
      if (it->size() <= 2) {
        it = routes.erase(it);
      } else {
        ++it;
      }
    }
  }
}
