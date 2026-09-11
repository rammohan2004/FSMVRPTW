#ifndef ROUTE_UTILS_H
#define ROUTE_UTILS_H

#include <string>
#include <vector>

#include "vrp.h"

double calculate_route_distance(const VRP &vrp, const std::vector<node_t> &route);
double calculate_total_cost(const VRP &vrp,
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
