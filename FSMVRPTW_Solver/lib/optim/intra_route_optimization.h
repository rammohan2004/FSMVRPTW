#ifndef INTRA_ROUTE_OPTIMIZATION_H
#define INTRA_ROUTE_OPTIMIZATION_H

#include <vector>

#include "../vrp.h"

void tsp_approx(const VRP &vrp,
                std::vector<node_t> &cities,
                std::vector<node_t> &tour,
                node_t ncities);
std::vector<std::vector<node_t>> postprocess_tsp_approx(
    const VRP &vrp, std::vector<std::vector<node_t>> &solRoutes);
void tsp_2opt(const VRP &vrp,
              std::vector<node_t> &cities,
              std::vector<node_t> &tour,
              unsigned ncities);
std::vector<std::vector<node_t>> postprocess_2OPT(
    const VRP &vrp, std::vector<std::vector<node_t>> &final_routes);
std::vector<std::vector<node_t>> postProcessIt(
    const VRP &vrp, std::vector<std::vector<node_t>> &final_routes, weight_t &minCost);
std::vector<std::vector<node_t>> postProcessIt_parallel(
    const VRP &vrp, std::vector<std::vector<node_t>> &final_routes, weight_t &minCost);
    
std::vector<std::vector<node_t>> postprocess_tsp_approx_parallel(
    const VRP &vrp, std::vector<std::vector<node_t>> &solRoutes);
std::vector<std::vector<node_t>> postprocess_2OPT_parallel(
    const VRP &vrp, std::vector<std::vector<node_t>> &final_routes);


    #endif
