#include "intra_route_optimization.h"

#include <cfloat>
#include <iostream>
#include <vector>

#include "../route_utils.h"

using namespace std;

void tsp_approx(const VRP &vrp,
                vector<node_t> &cities,
                vector<node_t> &tour,
                node_t ncities) {
  node_t ClosePt = 0;
  weight_t CloseDist;

  for (node_t i = 1; i < ncities; i++) {
    tour[i] = cities[i - 1];
  }
  tour[0] = cities[ncities - 1];

  double bestDistance = calculate_tour_distance_t(vrp, tour, ncities);

  for (node_t i = 1; i < ncities; i++) {
    weight_t ThisX = vrp.node[tour[i - 1]].x;
    weight_t ThisY = vrp.node[tour[i - 1]].y;
    CloseDist = DBL_MAX;
    
    for (node_t j = ncities - 1;; j--) {
      weight_t ThisDist =
          (vrp.node[tour[j]].x - ThisX) * (vrp.node[tour[j]].x - ThisX);
      if (ThisDist <= CloseDist) {
        ThisDist += (vrp.node[tour[j]].y - ThisY) * (vrp.node[tour[j]].y - ThisY);
        if (ThisDist <= CloseDist) {
          if (j < i) {
            break;
          }
          CloseDist = ThisDist;
          ClosePt = j;
        }
      }
    }

    unsigned temp = tour[i];
    tour[i] = tour[ClosePt];
    tour[ClosePt] = temp;

    double newDistance = calculate_tour_distance_t(vrp, tour, ncities);
    if (newDistance < bestDistance && verify_tour_t(vrp, tour, ncities)) {
      
      // THREAD SAFETY: Prevent console text from garbling
      // #pragma omp critical
      // {
      //   // cout << "TSP Approx Improvement: " << bestDistance << " to " << newDistance << endl;
      // }
      
      bestDistance = newDistance;
    } else {
      temp = tour[i];
      tour[i] = tour[ClosePt];
      tour[ClosePt] = temp;
    }
  }
}

// The customers of a route, with every depot marker removed.
//
// tsp_approx and tsp_2opt both expect a CUSTOMERS-ONLY sequence: each adds the
// depot legs itself (tsp_2opt computes get_dist(DEPOT, cities[0]) and
// get_dist(cities[n-1], DEPOT); tsp_approx takes the depot as cities[ncities-1]
// and fixes it at tour[0]). Their callers, however, used to pass the whole
// depot-wrapped route, which broke both:
//
//   * tsp_approx received THREE depots -- the route's two plus one more
//     appended -- and nearest-neighbour pulled them together, since d(0,0) = 0
//     is the most attractive edge on offer. The caller's tour[1..sz] slice then
//     dropped one, producing routes like "0 0 305 ... 536": leading depot
//     doubled, trailing depot gone.
//   * tsp_2opt reversed segments spanning position 0, moving a depot into the
//     middle of the route.
//
// A route that loses its trailing depot still COSTS correctly, because
// calculate_route_distance adds the depot legs unconditionally. The real damage
// is to feasibility: verify_route walks the route element by element, so with no
// trailing depot it never checks the arrival time back at the depot, and a route
// that overruns the depot's due date passes as valid.
//
// This bug is inherited unchanged from the original CVRPTW code, where nothing
// tested for it. It surfaced here only because step 5 added a routing-cost
// recomputation that assumes depot-wrapped routes and so disagrees when one is
// malformed.
static vector<node_t> customers_of(const vector<node_t> &route) {
  vector<node_t> customers;
  customers.reserve(route.size());
  for (node_t v : route) {
    if (v != DEPOT) {
      customers.push_back(v);
    }
  }
  return customers;
}

// Re-wraps a customer sequence as a proper route [DEPOT, ..., DEPOT].
static vector<node_t> wrap_with_depots(const vector<node_t> &customers) {
  vector<node_t> route;
  route.reserve(customers.size() + 2);
  route.push_back(DEPOT);
  for (node_t v : customers) {
    route.push_back(v);
  }
  route.push_back(DEPOT);
  return route;
}

vector<vector<node_t>> postprocess_tsp_approx(
    const VRP &vrp, vector<vector<node_t>> &solRoutes) {
  vector<vector<node_t>> modifiedRoutes;

  unsigned nroutes = solRoutes.size();
  for (unsigned i = 0; i < nroutes; ++i) {
    vector<node_t> customers = customers_of(solRoutes[i]);
    unsigned n = customers.size();
    if (n == 0) {
      modifiedRoutes.push_back(wrap_with_depots(customers));
      continue;
    }

    // cities = customers followed by the depot; tsp_approx puts cities[n] at
    // tour[0] and reorders only tour[1..n], so the depot stays fixed at the
    // front and the customers are what get permuted.
    vector<node_t> cities(n + 1);
    vector<node_t> tour(n + 1);
    for (unsigned j = 0; j < n; ++j) {
      cities[j] = customers[j];
    }
    cities[n] = DEPOT;

    tsp_approx(vrp, cities, tour, n + 1);

    vector<node_t> ordered(tour.begin() + 1, tour.begin() + n + 1);
    modifiedRoutes.push_back(wrap_with_depots(ordered));
  }
  return modifiedRoutes;
}

vector<vector<node_t>> postprocess_tsp_approx_parallel(
    const VRP &vrp, vector<vector<node_t>> &solRoutes) {
  
  int nroutes = solRoutes.size();
  
  vector<vector<node_t>> modifiedRoutes(nroutes);

  // Same depot-contract fix as the sequential version above.
  #pragma omp parallel for schedule(dynamic)
  for (int i = 0; i < nroutes; ++i) {
    vector<node_t> customers = customers_of(solRoutes[i]);
    int n = static_cast<int>(customers.size());
    if (n == 0) {
      modifiedRoutes[i] = wrap_with_depots(customers);
      continue;
    }

    vector<node_t> cities(n + 1);
    vector<node_t> tour(n + 1);
    for (int j = 0; j < n; ++j) {
      cities[j] = customers[j];
    }
    cities[n] = DEPOT;

    tsp_approx(vrp, cities, tour, n + 1);

    vector<node_t> ordered(tour.begin() + 1, tour.begin() + n + 1);
    modifiedRoutes[i] = wrap_with_depots(ordered);
  }
  
  return modifiedRoutes;
}



void tsp_2opt(const VRP &vrp,
              vector<node_t> &cities,
              vector<node_t> &tour,
              unsigned ncities) {
  unsigned improve = 0;

  while (improve < 2) {
    double best_distance = 0.0;
    best_distance += vrp.get_dist(DEPOT, cities[0]);
    for (unsigned jj = 1; jj < ncities; ++jj) {
      best_distance += vrp.get_dist(cities[jj - 1], cities[jj]);
    }
    best_distance += vrp.get_dist(cities[ncities - 1], DEPOT);

    for (unsigned i = 0; i < ncities - 1; ++i) {
      for (unsigned k = i + 1; k < ncities; ++k) {
        for (unsigned c = 0; c < i; ++c) {
          tour[c] = cities[c];
        }

        unsigned dec = 0;
        for (unsigned c = i; c <= k; ++c) {
          tour[c] = cities[k - dec];
          ++dec;
        }

        for (unsigned c = k + 1; c < ncities; ++c) {
          tour[c] = cities[c];
        }

        double new_distance = 0.0;
        new_distance += vrp.get_dist(DEPOT, tour[0]);
        for (unsigned jj = 1; jj < ncities; ++jj) {
          new_distance += vrp.get_dist(tour[jj - 1], tour[jj]);
        }
        new_distance += vrp.get_dist(tour[ncities - 1], DEPOT);

        vector<node_t> tmp_tour_with_depot;
        tmp_tour_with_depot.reserve(ncities + 1);
        tmp_tour_with_depot.push_back(DEPOT);
        for (unsigned t = 0; t < ncities; ++t) {
          tmp_tour_with_depot.push_back(tour[t]);
        }

        if (new_distance < best_distance &&
            verify_tour_t(vrp, tmp_tour_with_depot, ncities + 1)) {
            
          // --- THREAD SAFETY: Prevent console text from garbling ---
          // #pragma omp critical
          // {
          //   cout << "2OPT Improvement: " << best_distance << " to " << new_distance
          //        << endl;
          // }
          
          improve = 0;
          for (unsigned jj = 0; jj < ncities; ++jj) {
            cities[jj] = tour[jj];
          }
          best_distance = new_distance; // Ensure best_distance is updated!
        }
      }
    }
    ++improve;
  }
}

vector<vector<node_t>> postprocess_2OPT(
    const VRP &vrp, vector<vector<node_t>> &final_routes) {
  vector<vector<node_t>> postprocessed_final_routes;

  unsigned nroutes = final_routes.size();
  for (unsigned i = 0; i < nroutes; ++i) {
    // tsp_2opt adds the depot legs itself, so it must be given customers only.
    // Passing the depot-wrapped route let a segment reversal spanning position 0
    // move a depot into the middle of the route.
    vector<node_t> cities = customers_of(final_routes[i]);
    unsigned n = cities.size();
    vector<node_t> tour(n);

    if (n > 1) {
      tsp_2opt(vrp, cities, tour, n);
    }

    postprocessed_final_routes.push_back(wrap_with_depots(cities));
  }
  return postprocessed_final_routes;
}

vector<vector<node_t>> postprocess_2OPT_parallel(
    const VRP &vrp, vector<vector<node_t>> &final_routes) {
    
  int nroutes = final_routes.size();
  
  vector<vector<node_t>> postprocessed_final_routes(nroutes);

  // Same depot-contract fix as the sequential version above.
  #pragma omp parallel for schedule(dynamic)
  for (int i = 0; i < nroutes; ++i) {
    vector<node_t> cities = customers_of(final_routes[i]);
    unsigned n = cities.size();
    vector<node_t> tour(n);

    if (n > 1) {
      tsp_2opt(vrp, cities, tour, n);
    }

    postprocessed_final_routes[i] = wrap_with_depots(cities);
  }
  
  return postprocessed_final_routes;
}




vector<vector<node_t>> postProcessIt_parallel(
    const VRP &vrp, vector<vector<node_t>> &final_routes, weight_t &minCost) {
  vector<vector<node_t>> postprocessed_final_routes;

  auto postprocessed_final_routes1 = postprocess_tsp_approx_parallel(vrp, final_routes);
  // auto postprocessed_final_routes1 = postprocess_tsp_approx(vrp, final_routes);
  if (verify_route_t(vrp, postprocessed_final_routes1)) {
    cout << "\nPostprocess 1 route valid" << endl;
  } else {
    cout << "\nPostprocess 1 route invalid" << endl;
  }

  auto postprocessed_final_routes2 = postprocess_2OPT_parallel(vrp, postprocessed_final_routes1);
  // auto postprocessed_final_routes2 = postprocess_2OPT(vrp, postprocessed_final_routes1);
  if (verify_route_t(vrp, postprocessed_final_routes2)) {
    cout << "Postprocess 2 route valid" << endl;
  } else {
    cout << "Postprocess 2 route invalid" << endl;
  }

  auto postprocessed_final_routes3 = postprocess_2OPT_parallel(vrp, final_routes);
  // auto postprocessed_final_routes3 = postprocess_2OPT(vrp, final_routes);
  if (verify_route_t(vrp, postprocessed_final_routes3)) {
    cout << "Postprocess 3 route valid" << endl;
  } else {
    cout << "Postprocess 3 route invalid" << endl;
  }

  weight_t postprocessed_final_routes_cost = 0;
  for (unsigned zzz = 0; zzz < final_routes.size(); ++zzz) {
    vector<node_t> postprocessed_route2 = postprocessed_final_routes2[zzz];
    vector<node_t> postprocessed_route3 = postprocessed_final_routes3[zzz];

    unsigned sz2 = postprocessed_route2.size();
    unsigned sz3 = postprocessed_route3.size();

    weight_t postprocessed_route2_cost = 0.0;
    postprocessed_route2_cost += vrp.get_dist(DEPOT, postprocessed_route2[0]);
    for (unsigned jj = 1; jj < sz2; ++jj) {
      postprocessed_route2_cost +=
          vrp.get_dist(postprocessed_route2[jj - 1], postprocessed_route2[jj]);
    }
    postprocessed_route2_cost += vrp.get_dist(DEPOT, postprocessed_route2[sz2 - 1]);

    weight_t postprocessed_route3_cost = 0.0;
    postprocessed_route3_cost += vrp.get_dist(DEPOT, postprocessed_route3[0]);
    for (unsigned jj = 1; jj < sz3; ++jj) {
      postprocessed_route3_cost +=
          vrp.get_dist(postprocessed_route3[jj - 1], postprocessed_route3[jj]);
    }
    postprocessed_route3_cost += vrp.get_dist(DEPOT, postprocessed_route3[sz3 - 1]);

    if (postprocessed_route3_cost > postprocessed_route2_cost) {
      postprocessed_final_routes_cost += postprocessed_route2_cost;
      postprocessed_final_routes.push_back(postprocessed_route2);
    } else {
      postprocessed_final_routes_cost += postprocessed_route3_cost;
      postprocessed_final_routes.push_back(postprocessed_route3);
    }
  }

  minCost = postprocessed_final_routes_cost;
  return postprocessed_final_routes;
}

vector<vector<node_t>> postProcessIt(
    const VRP &vrp, vector<vector<node_t>> &final_routes, weight_t &minCost) {
  vector<vector<node_t>> postprocessed_final_routes;

  auto postprocessed_final_routes1 = postprocess_tsp_approx(vrp, final_routes);
  if (verify_route_t(vrp, postprocessed_final_routes1)) {
    cout << "\nPostprocess 1 route valid" << endl;
  } else {
    cout << "\nPostprocess 1 route invalid" << endl;
  }

  auto postprocessed_final_routes2 = postprocess_2OPT(vrp, postprocessed_final_routes1);
  if (verify_route_t(vrp, postprocessed_final_routes2)) {
    cout << "Postprocess 2 route valid" << endl;
  } else {
    cout << "Postprocess 2 route invalid" << endl;
  }

  auto postprocessed_final_routes3 = postprocess_2OPT(vrp, final_routes);
  if (verify_route_t(vrp, postprocessed_final_routes3)) {
    cout << "Postprocess 3 route valid" << endl;
  } else {
    cout << "Postprocess 3 route invalid" << endl;
  }

  weight_t postprocessed_final_routes_cost = 0;
  for (unsigned zzz = 0; zzz < final_routes.size(); ++zzz) {
    vector<node_t> postprocessed_route2 = postprocessed_final_routes2[zzz];
    vector<node_t> postprocessed_route3 = postprocessed_final_routes3[zzz];

    unsigned sz2 = postprocessed_route2.size();
    unsigned sz3 = postprocessed_route3.size();

    weight_t postprocessed_route2_cost = 0.0;
    postprocessed_route2_cost += vrp.get_dist(DEPOT, postprocessed_route2[0]);
    for (unsigned jj = 1; jj < sz2; ++jj) {
      postprocessed_route2_cost +=
          vrp.get_dist(postprocessed_route2[jj - 1], postprocessed_route2[jj]);
    }
    postprocessed_route2_cost += vrp.get_dist(DEPOT, postprocessed_route2[sz2 - 1]);

    weight_t postprocessed_route3_cost = 0.0;
    postprocessed_route3_cost += vrp.get_dist(DEPOT, postprocessed_route3[0]);
    for (unsigned jj = 1; jj < sz3; ++jj) {
      postprocessed_route3_cost +=
          vrp.get_dist(postprocessed_route3[jj - 1], postprocessed_route3[jj]);
    }
    postprocessed_route3_cost += vrp.get_dist(DEPOT, postprocessed_route3[sz3 - 1]);

    if (postprocessed_route3_cost > postprocessed_route2_cost) {
      postprocessed_final_routes_cost += postprocessed_route2_cost;
      postprocessed_final_routes.push_back(postprocessed_route2);
    } else {
      postprocessed_final_routes_cost += postprocessed_route3_cost;
      postprocessed_final_routes.push_back(postprocessed_route3);
    }
  }

  minCost = postprocessed_final_routes_cost;
  return postprocessed_final_routes;
}

