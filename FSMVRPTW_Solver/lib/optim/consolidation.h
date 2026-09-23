#ifndef CONSOLIDATION_H
#define CONSOLIDATION_H

#include <vector>

#include "../vrp.h"

// Route elimination by redistribution -- the move the rest of the local search
// cannot express.
//
// Picks a route, tries to reinsert EVERY one of its customers individually into
// other routes, and if all of them fit feasibly and the arithmetic pays, deletes
// the route outright. Eliminating a route saves its whole fixed cost F(z) plus
// all of its travel, which on these instances dwarfs anything a distance-only
// move can win.
//
// Why this and not a better Clarke-Wright savings term (step 7, which failed):
// Clarke-Wright merges by CONCATENATING two routes end to end, which preserves
// each route's internal order and therefore can never interleave their
// customers. Under wide time windows a long route must thread its customers in
// time order, which requires interleaving -- so no scoring of concatenations can
// produce one. Reinserting customers ONE AT A TIME, each at its own best
// position, is exactly that missing interleaving.
//
// Returns true if at least one route was eliminated.

// The K nearest customers of every customer, by distance.
//
// This is what bounds the insertion search: a displaced customer is only tried
// at positions adjacent to one of its K nearest neighbours, which keeps the move
// near-linear in fleet size instead of scanning every position of every route.
//
// Built ONCE per instance and passed in, because it depends only on the instance
// geometry and never on the current routes. It used to be rebuilt inside every
// consolidate_routes call, and the driver calls that up to 10 times -- O(n^2)
// each, which is 100 billion wasted operations at 100,000 customers.
std::vector<std::vector<node_t>> build_neighbour_lists(const VRP &vrp, int K);

bool consolidate_routes(const VRP &vrp,
                        std::vector<std::vector<node_t>> &routes,
                        const std::vector<std::vector<node_t>> &knn);

#endif
