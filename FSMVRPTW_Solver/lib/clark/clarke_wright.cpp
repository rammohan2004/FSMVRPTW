#include "clarke_wright.h"
#include "../route_utils.h"
#include <omp.h>
#include <algorithm>
#include <iostream>
#include <vector>

using namespace std;

vector<vector<node_t>> clarke_wright_cvrptw(
    const VRP &vrp, const vector<vector<int>> &clusters) {
      cout<<"Running sequential Clarke & Wright..."<<endl;
  double alpha = 0.7;
  double beta = 0.3;

  vector<vector<node_t>> final_routes;

  auto compute_arrival_time = [&](const vector<node_t> &route) {
    double time = 0.0;
    node_t prev = 0;

    for (node_t node : route) {
      time += vrp.get_dist(prev, node);
      if (time < vrp.node[node].earlyTime) {
        time = vrp.node[node].earlyTime;
      }
      if (time > vrp.node[node].latestTime) {
        return -1.0;
      }
      time += vrp.node[node].serviceTime;
      prev = node;
    }

    return time;
  };

  auto verify_route = [&](const vector<node_t> &route) {
    return compute_arrival_time(route) >= 0;
  };

  for (const auto &cluster : clusters) {
    vector<vector<node_t>> routes;
    vector<double> route_demand;

    for (auto node : cluster) {
      routes.push_back({node});
      route_demand.push_back(vrp.node[node].demand);
    }

    while (true) {
      double best_saving = -1e18;
      int best_i = -1;
      int best_j = -1;
      vector<node_t> best_merge;

      for (size_t r_i = 0; r_i < routes.size(); r_i++) {
        if (routes[r_i].empty()) {
          continue;
        }

        for (size_t r_j = r_i + 1; r_j < routes.size(); r_j++) {
          if (routes[r_j].empty()) {
            continue;
          }

          if (route_demand[r_i] + route_demand[r_j] > vrp.getCapacity()) {
            continue;
          }

          auto &Ri = routes[r_i];
          auto &Rj = routes[r_j];

          node_t i1 = Ri.front();
          node_t i2 = Ri.back();
          node_t j1 = Rj.front();
          node_t j2 = Rj.back();

          double arrival_i_end = compute_arrival_time(Ri);
          double arrival_j_end = compute_arrival_time(Rj);
          if (arrival_i_end < 0 || arrival_j_end < 0) {
            continue;
          }

          struct Candidate {
            node_t from;
            node_t to;
            vector<node_t> merged;
          };

          vector<Candidate> candidates;

          {
            vector<node_t> merged = Ri;
            merged.insert(merged.end(), Rj.begin(), Rj.end());
            candidates.push_back({i2, j1, merged});
          }

          {
            vector<node_t> merged = Rj;
            merged.insert(merged.end(), Ri.begin(), Ri.end());
            candidates.push_back({j2, i1, merged});
          }

          {
            vector<node_t> Ri_rev = Ri;
            reverse(Ri_rev.begin(), Ri_rev.end());
            vector<node_t> merged = Ri_rev;
            merged.insert(merged.end(), Rj.begin(), Rj.end());
            candidates.push_back({i1, j1, merged});
          }

          {
            vector<node_t> Rj_rev = Rj;
            reverse(Rj_rev.begin(), Rj_rev.end());
            vector<node_t> merged = Ri;
            merged.insert(merged.end(), Rj_rev.begin(), Rj_rev.end());
            candidates.push_back({i2, j2, merged});
          }

          for (auto &cand : candidates) {
            node_t from = cand.from;
            node_t to = cand.to;

            double arrival_from =
                compute_arrival_time((from == i2 || from == i1) ? Ri : Rj);
            if (arrival_from < 0) {
              continue;
            }

            double dist_saving = vrp.get_dist(0, from) + vrp.get_dist(0, to) -
                                 vrp.get_dist(from, to);

            double arrival_to = arrival_from + vrp.get_dist(from, to);
            double waiting = 0.0;
            if (arrival_to < vrp.node[to].earlyTime) {
              waiting = vrp.node[to].earlyTime - arrival_to;
            }

            double total_saving = alpha * dist_saving - beta * waiting;
            if (!verify_route(cand.merged)) {
              continue;
            }

            if (total_saving > best_saving) {
              best_saving = total_saving;
              best_i = static_cast<int>(r_i);
              best_j = static_cast<int>(r_j);
              best_merge = cand.merged;
            }
          }
        }
      }

      if (best_saving <= 0) {
        break;
      }

      routes[best_i] = best_merge;
      route_demand[best_i] += route_demand[best_j];
      routes[best_j].clear();
      route_demand[best_j] = 0;
    }

    for (auto &route : routes) {
      if (!route.empty()) {
        final_routes.push_back(route);
      }
    }
  }

  return final_routes;
}


// Parrale version of clark-wright using OpenMP.
//The main idea is to parallelize the innermost loop 
// where we evaluate all pairs of routes for potential merging. 
// Each thread will keep track of its own best merge candidate and update the global variable.

vector<vector<node_t>> clarke_wright_cvrptw_parallel(
    const VRP &vrp, const vector<vector<int>> &clusters) {
  
  double alpha = 0.7;
  double beta = 0.3;

  vector<vector<node_t>> final_routes;

  
  auto compute_arrival_time = [&](const vector<node_t> &route) {
    double time = 0.0;
    node_t prev = 0; 

    for (node_t node : route) {
      time += vrp.get_dist(prev, node);
      
      
      if (time < vrp.node[node].earlyTime) {
        time = vrp.node[node].earlyTime;
      }
      
      if (time > vrp.node[node].latestTime) {
        return -1.0;
      }
      
      time += vrp.node[node].serviceTime;
      prev = node;
    }

    return time;
  };

  auto verify_route = [&](const vector<node_t> &route) {
    return compute_arrival_time(route) >= 0;
  };

 
  for (const auto &cluster : clusters) {
    vector<vector<node_t>> routes;
    vector<double> route_demand;

    
    for (auto node : cluster) {
      routes.push_back({node});
      route_demand.push_back(vrp.node[node].demand);
    }

    
    while (true) {
      double global_best_saving = -1e18;
      int global_best_i = -1;
      int global_best_j = -1;
      vector<node_t> global_best_merge;

      
      #pragma omp parallel 
      {
        
        double local_best_saving = -1e18;
        int local_best_i = -1;
        int local_best_j = -1;
        vector<node_t> local_best_merge;

       
        #pragma omp for schedule(dynamic) nowait
        for (int r_i = 0; r_i < static_cast<int>(routes.size()); r_i++) {
          if (routes[r_i].empty()) continue;

          for (int r_j = r_i + 1; r_j < static_cast<int>(routes.size()); r_j++) {
            if (routes[r_j].empty()) continue;
            
            
            if (route_demand[r_i] + route_demand[r_j] > vrp.getCapacity()) {
              continue;
            }

            auto &Ri = routes[r_i];
            auto &Rj = routes[r_j];

            node_t i1 = Ri.front();
            node_t i2 = Ri.back();
            node_t j1 = Rj.front();
            node_t j2 = Rj.back();

            double arrival_i_end = compute_arrival_time(Ri);
            double arrival_j_end = compute_arrival_time(Rj);
            if (arrival_i_end < 0 || arrival_j_end < 0) continue;

            struct Candidate {
              node_t from, to;
              vector<node_t> merged;
            };

            vector<Candidate> candidates;

            // Option 1: Ri -> Rj
            {
              vector<node_t> merged = Ri;
              merged.insert(merged.end(), Rj.begin(), Rj.end());
              candidates.push_back({i2, j1, merged});
            }

            // Option 2: Rj -> Ri
            {
              vector<node_t> merged = Rj;
              merged.insert(merged.end(), Ri.begin(), Ri.end());
              candidates.push_back({j2, i1, merged});
            }

            // Option 3: Reversed(Ri) -> Rj
            {
              vector<node_t> Ri_rev = Ri;
              reverse(Ri_rev.begin(), Ri_rev.end());
              vector<node_t> merged = Ri_rev;
              merged.insert(merged.end(), Rj.begin(), Rj.end());
              candidates.push_back({i1, j1, merged});
            }

            // Option 4: Ri -> Reversed(Rj)
            {
              vector<node_t> Rj_rev = Rj;
              reverse(Rj_rev.begin(), Rj_rev.end());
              vector<node_t> merged = Ri;
              merged.insert(merged.end(), Rj_rev.begin(), Rj_rev.end());
              candidates.push_back({i2, j2, merged});
            }

            // Evaluate all valid candidate merges
            for (auto &cand : candidates) {
              node_t from = cand.from;
              node_t to = cand.to;

              double arrival_from = compute_arrival_time((from == i2 || from == i1) ? Ri : Rj);
              if (arrival_from < 0) continue;

              double dist_saving = vrp.get_dist(0, from) + vrp.get_dist(0, to) - vrp.get_dist(from, to);
              double arrival_to = arrival_from + vrp.get_dist(from, to);
              double waiting = 0.0;
              
              if (arrival_to < vrp.node[to].earlyTime) {
                waiting = vrp.node[to].earlyTime - arrival_to;
              }

              double total_saving = alpha * dist_saving - beta * waiting;
              
              
              if (!verify_route(cand.merged)) continue;

              
              if (total_saving > local_best_saving) {
                local_best_saving = total_saving;
                local_best_i = r_i;
                local_best_j = r_j;
                local_best_merge = cand.merged;
              }
            }
          }
        }

        // --- GLOBAL SYNCHRONIZATION ---
        #pragma omp critical
        {
          if (local_best_saving > global_best_saving) {
            global_best_saving = local_best_saving;
            global_best_i = local_best_i;
            global_best_j = local_best_j;
            global_best_merge = std::move(local_best_merge);
          }
        }
      } 
      // --- OPENMP PARALLEL REGION ENDS ---

      
      if (global_best_saving <= 0) {
        break;
      }

      
      routes[global_best_i] = std::move(global_best_merge);
      route_demand[global_best_i] += route_demand[global_best_j];
      routes[global_best_j].clear(); 
      route_demand[global_best_j] = 0;
    }

    
    for (auto &route : routes) {
      if (!route.empty()) {
        final_routes.push_back(route);
      }
    }
  }

  return final_routes;
}

vector<vector<node_t>> clarke_wright_cvrptw_parallel_v2(
    const VRP &vrp, const vector<vector<int>> &clusters) {
  
  double alpha = 0.7;
  double beta = 0.3;

  auto compute_arrival_time = [&](const vector<node_t> &route) {
    double time = 0.0;
    node_t prev = 0;

    for (node_t node : route) {
      time += vrp.get_dist(prev, node);
      if (time < vrp.node[node].earlyTime) {
        time = vrp.node[node].earlyTime;
      }
      if (time > vrp.node[node].latestTime) {
        return -1.0;
      }
      time += vrp.node[node].serviceTime;
      prev = node;
    }

    return time;
  };

  auto verify_route = [&](const vector<node_t> &route) {
    return compute_arrival_time(route) >= 0;
  };

  int num_clusters = static_cast<int>(clusters.size());
  
  vector<vector<vector<node_t>>> cluster_results(num_clusters);

  #pragma omp parallel for schedule(dynamic)
  for (int c = 0; c < num_clusters; ++c) {
    const auto &cluster = clusters[c];
    
    vector<vector<node_t>> routes;
    vector<double> route_demand;

    for (auto node : cluster) {
      routes.push_back({node});
      route_demand.push_back(vrp.node[node].demand);
    }

    while (true) {
      double best_saving = -1e18;
      int best_i = -1;
      int best_j = -1;
      vector<node_t> best_merge;

      for (size_t r_i = 0; r_i < routes.size(); r_i++) {
        if (routes[r_i].empty()) {
          continue;
        }

        for (size_t r_j = r_i + 1; r_j < routes.size(); r_j++) {
          if (routes[r_j].empty()) {
            continue;
          }

          if (route_demand[r_i] + route_demand[r_j] > vrp.getCapacity()) {
            continue;
          }

          auto &Ri = routes[r_i];
          auto &Rj = routes[r_j];

          node_t i1 = Ri.front();
          node_t i2 = Ri.back();
          node_t j1 = Rj.front();
          node_t j2 = Rj.back();

          double arrival_i_end = compute_arrival_time(Ri);
          double arrival_j_end = compute_arrival_time(Rj);
          if (arrival_i_end < 0 || arrival_j_end < 0) {
            continue;
          }

          struct Candidate {
            node_t from;
            node_t to;
            vector<node_t> merged;
          };

          vector<Candidate> candidates;

          {
            vector<node_t> merged = Ri;
            merged.insert(merged.end(), Rj.begin(), Rj.end());
            candidates.push_back({i2, j1, merged});
          }

          {
            vector<node_t> merged = Rj;
            merged.insert(merged.end(), Ri.begin(), Ri.end());
            candidates.push_back({j2, i1, merged});
          }

          {
            vector<node_t> Ri_rev = Ri;
            reverse(Ri_rev.begin(), Ri_rev.end());
            vector<node_t> merged = Ri_rev;
            merged.insert(merged.end(), Rj.begin(), Rj.end());
            candidates.push_back({i1, j1, merged});
          }

          {
            vector<node_t> Rj_rev = Rj;
            reverse(Rj_rev.begin(), Rj_rev.end());
            vector<node_t> merged = Ri;
            merged.insert(merged.end(), Rj_rev.begin(), Rj_rev.end());
            candidates.push_back({i2, j2, merged});
          }

          for (auto &cand : candidates) {
            node_t from = cand.from;
            node_t to = cand.to;

            double arrival_from =
                compute_arrival_time((from == i2 || from == i1) ? Ri : Rj);
            if (arrival_from < 0) {
              continue;
            }

            double dist_saving = vrp.get_dist(0, from) + vrp.get_dist(0, to) -
                                 vrp.get_dist(from, to);

            double arrival_to = arrival_from + vrp.get_dist(from, to);
            double waiting = 0.0;
            if (arrival_to < vrp.node[to].earlyTime) {
              waiting = vrp.node[to].earlyTime - arrival_to;
            }

            double total_saving = alpha * dist_saving - beta * waiting;
            if (!verify_route(cand.merged)) {
              continue;
            }

            if (total_saving > best_saving) {
              best_saving = total_saving;
              best_i = static_cast<int>(r_i);
              best_j = static_cast<int>(r_j);
              best_merge = cand.merged;
            }
          }
        }
      }

      if (best_saving <= 0) {
        break;
      }

      routes[best_i] = best_merge;
      route_demand[best_i] += route_demand[best_j];
      routes[best_j].clear();
      route_demand[best_j] = 0;
    }

    for (auto &route : routes) {
      if (!route.empty()) {
        cluster_results[c].push_back(route);
      }
    }
  }

  vector<vector<node_t>> final_routes;
  for (int c = 0; c < num_clusters; ++c) {
    for (auto &route : cluster_results[c]) {
      final_routes.push_back(std::move(route));
    }
  }

  return final_routes;
}

vector<vector<node_t>> clarke_wright_cvrptw_distance(
    const VRP &vrp, const std::vector<std::vector<int>> &clusters) {
  const int num_clusters = static_cast<int>(clusters.size());
  vector<vector<vector<node_t>>> cluster_results(num_clusters);

#pragma omp parallel for schedule(dynamic)
  for (int c = 0; c < num_clusters; ++c) {
    const auto &cluster = clusters[c];
    const size_t cluster_size = cluster.size();
    struct DistanceSaving {
      node_t i;
      node_t j;
      weight_t value;
    };

    vector<vector<node_t>> routes;
    vector<demand_t> route_demand;
    vector<int> node_to_route(vrp.getSize(), -1);
    routes.reserve(cluster_size);
    route_demand.reserve(cluster_size);

    for (node_t node : cluster) {
      routes.push_back({node});
      route_demand.push_back(vrp.node[node].demand);
      node_to_route[node] = static_cast<int>(routes.size()) - 1;
    }

    vector<DistanceSaving> savings;
    savings.reserve(cluster_size * (cluster_size > 1 ? cluster_size - 1 : 0) / 2);

    for (size_t i = 0; i < cluster_size; ++i) {
      for (size_t j = i + 1; j < cluster_size; ++j) {
        const node_t from = cluster[i];
        const node_t to = cluster[j];
        const weight_t saving =
            vrp.get_dist(DEPOT, from) + vrp.get_dist(DEPOT, to) -
            vrp.get_dist(from, to);
        savings.push_back({from, to, saving});
      }
    }

    sort(savings.begin(), savings.end(),
         [](const DistanceSaving &a, const DistanceSaving &b) {
           return a.value > b.value;
         });

    for (const auto &saving : savings) {
      const node_t i = saving.i;
      const node_t j = saving.j;

      const int r_i = node_to_route[i];
      const int r_j = node_to_route[j];
      if (r_i < 0 || r_j < 0 || r_i == r_j) {
        continue;
      }

      if (route_demand[r_i] + route_demand[r_j] > vrp.getCapacity()) {
        continue;
      }

      auto route_i = routes[r_i];
      auto route_j = routes[r_j];
      if (route_i.empty() || route_j.empty()) {
        continue;
      }

      const bool i_start = route_i.front() == i;
      const bool i_end = route_i.back() == i;
      const bool j_start = route_j.front() == j;
      const bool j_end = route_j.back() == j;

      if (!(i_start || i_end) || !(j_start || j_end)) {
        continue;
      }

      vector<node_t> merged;
      if (i_end && j_start) {
        merged = route_i;
        merged.insert(merged.end(), route_j.begin(), route_j.end());
      } else if (i_start && j_end) {
        merged = route_j;
        merged.insert(merged.end(), route_i.begin(), route_i.end());
      } else if (i_end && j_end) {
        reverse(route_j.begin(), route_j.end());
        merged = route_i;
        merged.insert(merged.end(), route_j.begin(), route_j.end());
      } else if (i_start && j_start) {
        reverse(route_i.begin(), route_i.end());
        merged = route_i;
        merged.insert(merged.end(), route_j.begin(), route_j.end());
      } else {
        continue;
      }

      if (!verify_single_route(vrp, merged)) {
        continue;
      }

      routes[r_i] = std::move(merged);
      route_demand[r_i] += route_demand[r_j];

      for (node_t node : routes[r_j]) {
        node_to_route[node] = r_i;
      }

      routes[r_j].clear();
      route_demand[r_j] = 0;
    }

    for (auto &route : routes) {
      if (!route.empty()) {
        cluster_results[c].push_back(std::move(route));
      }
    }
  }

  vector<vector<node_t>> final_routes;
  for (int c = 0; c < num_clusters; ++c) {
    for (auto &route : cluster_results[c]) {
      final_routes.push_back(std::move(route));
    }
  }

  return final_routes;
}
