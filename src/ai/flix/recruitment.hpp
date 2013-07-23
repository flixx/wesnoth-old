/*
   Copyright (C) 2013 by Felix Bauer
   Part of the Battle for Wesnoth Project http://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

/**
 * @file
 * Recruitment Engine by flix
 */

#ifndef AI_FLIX_RECRUITMENT_HPP_INCLUDED
#define AI_FLIX_RECRUITMENT_HPP_INCLUDED

#include "../composite/rca.hpp"
#include "../../unit.hpp"

#include <boost/foreach.hpp>
#include <boost/optional.hpp>
#include <iomanip>

#ifdef _MSC_VER
#pragma warning(push)
// silence "inherits via dominance" warnings
#pragma warning(disable:4250)
#endif

namespace pathfind {

struct full_cost_map;

} //of namespace pathfind

namespace ai {

namespace flix_recruitment {

// each leader have a score_map
// the score map indicates what is the best mix of own units from the leader point of view.
// The leader will then recruit according to the map.
typedef std::map<std::string, double> score_map;

// In the limit_map it is stored how much units we can recruit per unit type.
// Those limits come from configurations.
// Each leader will have a limit_map and there will also be a global limit_map for both leader.
// This is just a dummy yet and will depend on the configuration definitions.
typedef std::map<std::string, int> limit_map;

typedef std::map<t_translation::t_terrain, int> terrain_count_map;

struct data {
	unit_map::const_iterator leader;
	std::set<std::string> recruits;
	score_map scores;
	limit_map limits;

	// We use ratio_score to decide with which ratios the leaders recruit among each other.
	// For example if leader1 have a ratio_score of 1 and leader2 have a ratio_score of 2
	// then leader2 will recruit twice as much units than leader1.
	double ratio_score;

	int recruit_count;

	explicit data(const unit_map::const_iterator leader)
		: leader(leader), ratio_score(1.0), recruit_count(0) { }
	double get_score_sum() const {
		double sum = 0.0;
		BOOST_FOREACH(const score_map::value_type& entry, scores) {
			sum += entry.second;
		}
		return sum;
	}
	score_map get_normalized_scores() const {
		const double sum = get_score_sum();
		if (sum == 0.0) {
			return scores;
		}
		score_map normalized;
		BOOST_FOREACH(const score_map::value_type& entry, scores) {
			normalized[entry.first] = entry.second / sum;
		}
		return normalized;
	}
	std::string to_string() const {
		std::stringstream s;
		s << "---------------Content of leader data---------------\n";
		s << "For leader: " << leader->name() << "\n";
		s << "ratio_score: " << ratio_score << "\n";
		s << "recruit_count: " << recruit_count << "\n\n";
		BOOST_FOREACH(const score_map::value_type& entry, scores) {
			limit_map::const_iterator limit_it = limits.find(entry.first);
			int limit = (limit_it != limits.end()) ? (limit_it->second) : -1;
			s << std::setw(20) << entry.first <<
					" score: " << std::setw(7) << entry.second <<
					" limit: " << limit << "\n";
		}
		s << "----------------------------------------------------\n";
		return s.str();
	}
};

class recruitment : public candidate_action {
public:
	recruitment(rca_context &context, const config &cfg);
	virtual ~recruitment();
	virtual double evaluate();
	virtual void execute();
	void update_important_hexes();
	//Debug only
	void show_important_hexes() const;
private:
	void invalidate();
	const pathfind::full_cost_map get_cost_map_of_side(int side) const;
	void compare_cost_maps_and_update_important_hexes(
			const pathfind::full_cost_map& my_cost_map,
			const pathfind::full_cost_map& enemy_cost_map);
	void update_average_local_cost();
	void do_map_analysis(std::vector<data>* leader_data);
	double get_average_defense(const std::string& unit_type) const;
	void do_combat_analysis(std::vector<data>* leader_data);
	double compare_unit_types(const std::string& a, const std::string& b) const;
	void simulate_attack(
			const unit_type* const attacker, const unit_type* const defender,
			double attacker_defense, double defender_defense,
			double* damage_to_attacker, double* damage_to_defender) const;

	std::set<map_location> important_hexes_;
	terrain_count_map important_terrain_;
	std::map<map_location, double> average_local_cost_;

	// The CA Object will be persistent over turns.
	// optional_cheapest_unit_cost_ is updated in execute() and
	// used in evaluate().
	// Use boost::optional to check for a uninitialized state.
	boost::optional<int> optional_cheapest_unit_cost_;

};

}  // of namespace flix_recruitment

}  // of namespace ai

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif
