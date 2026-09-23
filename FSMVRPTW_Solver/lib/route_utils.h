#ifndef ROUTE_UTILS_H
#define ROUTE_UTILS_H

#include <string>
#include <vector>

#include "vrp.h"

double calculate_route_distance(const VRP &vrp, const std::vector<node_t> &route);

// True when a route carries no customers. Clarke-Wright filters its own empties
// out, but the local search can produce one by relocating the last customer out
// of a route, leaving [DEPOT, DEPOT]. Such a route uses no vehicle and must not
// be charged a fixed cost.
bool route_is_empty(const std::vector<node_t> &route);

// THE OBJECTIVE: total distance travelled + fixed cost of the vehicles used.
// Each non-empty route is charged F(load) -- the fixed cost of the cheapest
// vehicle type able to carry it.
double calculate_total_cost(const VRP &vrp,
                            const std::vector<std::vector<node_t>> &routes);

// The two halves of that objective, reported separately because every result we
// publish carries the split and the cuOpt sheet expects both columns.
double total_routing_cost(const VRP &vrp,
                          const std::vector<std::vector<node_t>> &routes);
double total_fixed_cost(const VRP &vrp,
                        const std::vector<std::vector<node_t>> &routes);

// Number of routes that actually carry customers -- the vehicle count.
int count_vehicles_used(const std::vector<std::vector<node_t>> &routes);

// How many vehicles of each type the solution uses, indexed like vrp.types().
std::vector<int> fleet_mix(const VRP &vrp,
                           const std::vector<std::vector<node_t>> &routes);

// Compact fleet composition, e.g. "A126B30C1". Same format the cuOpt results
// use, so the two solvers' output can sit in one sheet.
std::string mix_string(const VRP &vrp,
                       const std::vector<std::vector<node_t>> &routes);

// ---- verification ------------------------------------------------------
// Our own solver deserves more scrutiny than cuOpt's, not less: a bug that
// flatters our result is the worst outcome available.

// Route distance recomputed by a different path from calculate_route_distance:
// it sums consecutive pairs only, with no depot terms bolted on. Agreement
// between the two also confirms every route really is depot-wrapped.
double recompute_routing_cost(const VRP &vrp,
                              const std::vector<std::vector<node_t>> &routes);

// Every customer 1..n-1 served exactly once, and no route visiting a customer
// twice.
bool all_customers_visited_once(const VRP &vrp,
                                const std::vector<std::vector<node_t>> &routes);
bool verify_route(const VRP &vrp, const std::vector<std::vector<node_t>> &routes);
bool verify_single_route(const VRP &vrp, const std::vector<node_t> &route);
bool verify_tour_t(const VRP &vrp, const std::vector<node_t> &tour, node_t ncities);
bool verify_route_t(const VRP &vrp, const std::vector<std::vector<node_t>> &routes);
double calculate_tour_distance_t(const VRP &vrp,
                                 const std::vector<node_t> &tour,
                                 node_t ncities);
double compute_waiting_time(const VRP &vrp, const std::vector<node_t> &route);
void print_routes(const std::vector<std::vector<node_t>> &routes);
void save_routes_snapshot(const std::vector<std::vector<node_t>> &routes,
                          const std::string &filename);
int max_length_of_route(const std::vector<std::vector<node_t>> &routes);

#endif
