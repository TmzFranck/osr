#pragma once

#include <ostream>

#include "cista/containers/rtree.h"
#include "cista/reflection/printable.h"

#include "geo/box.h"
#include "geo/polyline.h"

#include "osr/ways.h"

#include "utl/cflow.h"
#include "utl/helpers/algorithm.h"
#include "utl/pairwise.h"

#include "osr/location.h"
#include "osr/routing/profile.h"

namespace osr {

template <typename T, typename Collection, typename Fn>
void till_the_end(T const& start,
                  Collection const& c,
                  direction const dir,
                  Fn&& fn) {
  if (dir == direction::kForward) {
    for (auto i = start; i != c.size(); ++i) {
      if (fn(c[i]) == utl::cflow::kBreak) {
        break;
      }
    }
  } else {
    for (auto j = 0U; j <= start; ++j) {
      auto i = start - j;
      if (fn(c[i]) == utl::cflow::kBreak) {
        break;
      }
    }
  }
}

struct node_candidate {
  bool valid() const { return node_ != node_idx_t::invalid(); }

  level_t lvl_{kNoLevel};
  direction way_dir_{direction::kForward};
  node_idx_t node_{node_idx_t::invalid()};
  double dist_to_node_{0.0};
  cost_t cost_{0U};
  cost_t offroad_cost_{0U};
  std::vector<geo::latlng> path_{};
};

struct way_candidate {
  friend bool operator<(way_candidate const& a, way_candidate const& b) {
    return a.dist_to_way_ < b.dist_to_way_;
  }

  double dist_to_way_;
  geo::latlng best_;
  std::size_t segment_idx_;
  location query_{};
  way_idx_t way_{way_idx_t::invalid()};
  node_candidate left_{}, right_{};
};

using match_t = std::vector<way_candidate>;
using match_view_t = std::span<way_candidate const>;

struct lookup {
  lookup(ways const&, std::filesystem::path, cista::mmap::protection);

  void build_rtree();

  cista::mmap mm(char const* file) {
    return cista::mmap{(p_ / file).generic_string().c_str(), mode_};
  }

  match_t match(location const& query,
                bool const reverse,
                direction const search_dir,
                double const max_match_distance,
                bitvec<node_idx_t> const* blocked,
                search_profile) const;

  template <typename Profile>
  match_t match(location const& query,
                bool const reverse,
                direction const search_dir,
                double max_match_distance,
                bitvec<node_idx_t> const* blocked) const {
    auto way_candidates = get_way_candidates<Profile>(
        query, reverse, search_dir, max_match_distance, blocked);
    auto i = 0U;
    while (way_candidates.empty() && i++ < 4U) {
      max_match_distance *= 2U;
      way_candidates = get_way_candidates<Profile>(query, reverse, search_dir,
                                                   max_match_distance, blocked);
    }
    return way_candidates;
  }

  template <typename Fn>
  void find(geo::box const& b, Fn&& fn) const {
    auto const min = b.min_.lnglat_float();
    auto const max = b.max_.lnglat_float();
    rtree_.search(min, max, [&](auto, auto, way_idx_t const w) {
      fn(w);
      return true;
    });
  }

  hash_set<node_idx_t> find_elevators(geo::box const& b) const;

  void insert(way_idx_t);

private:
  template <typename Profile>
  match_t get_way_candidates(location const& query,
                             bool const reverse,
                             direction const search_dir,
                             double const max_match_distance,
                             bitvec<node_idx_t> const* blocked) const {
    auto way_candidates = std::vector<way_candidate>{};
    auto const approx_distance_lng_degrees =
        geo::approx_distance_lng_degrees(query.pos_);
    auto const squared_max_dist = std::pow(max_match_distance, 2);
    find(geo::box{query.pos_, max_match_distance}, [&](way_idx_t const way) {
      auto wc = geo::approx_squared_distance_to_polyline<way_candidate>(
          query.pos_, ways_.way_polylines_[way], approx_distance_lng_degrees);
      if (wc.dist_to_way_ < squared_max_dist) {
        wc.dist_to_way_ = std::sqrt(wc.dist_to_way_);
        wc.query_ = query;
        wc.way_ = way;
        wc.left_ = find_next_node<Profile>(
            wc, query, direction::kBackward, query.lvl_, reverse, search_dir,
            blocked, approx_distance_lng_degrees);
        wc.right_ = find_next_node<Profile>(
            wc, query, direction::kForward, query.lvl_, reverse, search_dir,
            blocked, approx_distance_lng_degrees);
        if (wc.left_.valid() || wc.right_.valid()) {
          way_candidates.emplace_back(std::move(wc));
        }
      }
    });
    utl::sort(way_candidates);
    return way_candidates;
  }

  template <typename Profile>
  node_candidate find_next_node(way_candidate const& wc,
                                location const&,
                                direction const dir,
                                level_t const lvl,
                                bool const reverse,
                                direction const search_dir,
                                bitvec<node_idx_t> const* blocked,
                                double approx_distance_lng_degrees) const {
    auto const way_prop = ways_.r_->way_properties_[wc.way_];
    auto const edge_dir = reverse ? opposite(dir) : dir;
    auto const offroad_cost =
        Profile::way_cost(way_prop, flip(search_dir, edge_dir),
                          static_cast<distance_t>(wc.dist_to_way_));
    if (offroad_cost == kInfeasible) {
      return node_candidate{};
    }

    auto c = node_candidate{.lvl_ = lvl,
                            .way_dir_ = dir,
                            .dist_to_node_ = wc.dist_to_way_,
                            .cost_ = offroad_cost,
                            .offroad_cost_ = offroad_cost,
                            .path_ = {wc.best_}};
    auto const polyline = ways_.way_polylines_[wc.way_];
    auto const osm_nodes = ways_.way_osm_nodes_[wc.way_];

    till_the_end(
        wc.segment_idx_ + (dir == direction::kForward ? 1U : 0U),
        utl::zip(polyline, osm_nodes), dir, [&](auto&& x) {
          auto const& [pos, osm_node_idx] = x;

          auto const segment_dist = std::sqrt(geo::approx_squared_distance(
              c.path_.back(), pos, approx_distance_lng_degrees));
          c.dist_to_node_ += segment_dist;
          c.cost_ += Profile::way_cost(way_prop, flip(search_dir, edge_dir),
                                       static_cast<distance_t>(segment_dist));
          c.path_.push_back(pos);

          auto const way_node = ways_.find_node_idx(osm_node_idx);
          if (way_node.has_value() &&
              (blocked == nullptr || !blocked->test(*way_node))) {
            c.node_ = *way_node;
            return utl::cflow::kBreak;
          }

          return utl::cflow::kContinue;
        });

    if (reverse) {
      std::reverse(begin(c.path_), end(c.path_));
    }

    return c;
  }

  std::filesystem::path p_;
  cista::mmap::protection mode_;
  cista::mm_rtree<way_idx_t> rtree_;
  ways const& ways_;
};

}  // namespace osr