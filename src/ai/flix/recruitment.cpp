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

#include "recruitment.hpp"

#include "../actions.hpp"
#include "../composite/rca.hpp"
#include "../../game_display.hpp"
#include "../../log.hpp"
#include "../../map.hpp"
#include "../../map_label.hpp"
#include "../../pathfind/pathfind.hpp"
#include "../../resources.hpp"
#include "../../team.hpp"
#include "../../unit_map.hpp"
#include "../../unit_types.hpp"

#include <boost/foreach.hpp>

static lg::log_domain log_ai_flix("ai/flix");
#define LOG_AI_FLIX LOG_STREAM(info, log_ai_flix)
#define DBG_AI_FLIX LOG_STREAM(debug, log_ai_flix)
#define ERR_AI_FLIX LOG_STREAM(err, log_ai_flix)

#ifdef _MSC_VER
#pragma warning(push)
// silence "inherits via dominance" warnings
#pragma warning(disable:4250)
#endif

namespace ai {

namespace flix_recruitment {

namespace {
// define some tweakable things here which _could_ be extracted as a aspect
const static int MAP_UNIT_THRESHOLD = 5;
const static bool MAP_IGNORE_ZOC = true;
}

recruitment::recruitment(rca_context &context, const config &cfg)
		: candidate_action(context, cfg), optional_cheapest_unit_cost_() { }

double recruitment::evaluate() {
	const unit_map &units = *resources::units;
	const std::vector<unit_map::const_iterator> leaders = units.find_leaders(get_side());

	BOOST_FOREACH(const unit_map::const_iterator &leader, leaders) {
		if (leader == resources::units->end()) {
			return BAD_SCORE;
		}

		if (optional_cheapest_unit_cost_) {
			// Check Gold. But proceed if there is a unit with cost <= 0 (WML can do that)
			if (current_team().gold() < optional_cheapest_unit_cost_ &&
					optional_cheapest_unit_cost_ > 0) {
				// TODO(flix) && recruitment-list wasn't changed.
				return BAD_SCORE;
			}
		}
		std::set<map_location> checked_hexes;
		const map_location &loc = leader->get_location();
		checked_hexes.insert(loc);

		if (resources::game_map->is_keep(loc) &&
				count_free_hexes_in_castle(loc, checked_hexes) != 0) {
			return get_score();
		}
	}

	return BAD_SCORE;
}

void recruitment::execute() {
	LOG_AI_FLIX<< "Flix recruitment begin! \n";

	/*
	 * Step 0: Check which leaders can recruit and collect them in leader_data.
	 *         Also initialize some other stuff.
	 */

	const unit_map &units = *resources::units;
	const std::vector<unit_map::const_iterator> leaders = units.find_leaders(get_side());

	// this is the central datastructure with all score_tables in it.
	std::vector<data> leader_data;
	// a set of all possible recruits
	std::set<std::string> global_recruits;

	BOOST_FOREACH(const unit_map::const_iterator& leader, leaders) {
		const map_location &keep = leader->get_location();
		if(!resources::game_map->is_keep(keep)) {
			DBG_AI_FLIX << "Leader " << leader->name() << " is not on keep. \n";
			continue;
		}
		std::set<map_location> checked_hexes;
		checked_hexes.insert(keep);
		if(count_free_hexes_in_castle(keep, checked_hexes) <= 0) {
			DBG_AI_FLIX << "Leader " << leader->name() << "is on keep but no hexes are free \n";
			continue;
		}

		// leader can recruit.

		data data(leader);

		// add team recruits
		BOOST_FOREACH(const std::string& recruit, current_team().recruits()) {
			data.recruits.insert(recruit);
			data.scores[recruit] = 0.0;
			data.limits[recruit] = 99999;
			global_recruits.insert(recruit);
			const unit_type* const info = unit_types.find(recruit);
			if (!optional_cheapest_unit_cost_ || info->cost() < optional_cheapest_unit_cost_) {
				optional_cheapest_unit_cost_ = info->cost();
			}
		}

		// add extra recruits
		BOOST_FOREACH(const std::string& recruit, leader->recruits()) {
			data.recruits.insert(recruit);
			data.scores[recruit] = 0.0;
			data.limits[recruit] = 99999;
			global_recruits.insert(recruit);
			const unit_type* const info = unit_types.find(recruit);
			if (!optional_cheapest_unit_cost_ || info->cost() < optional_cheapest_unit_cost_) {
				optional_cheapest_unit_cost_ = info->cost();
			}
		}

		leader_data.push_back(data);
	}

	if (leader_data.empty()) {
		DBG_AI_FLIX << "No leader available for recruiting. \n";
		return;  // This CA is going to be blacklisted for this turn.
	}

	if (!optional_cheapest_unit_cost_) {
		// When it is uninitialized it must be that:
		DBG_AI_FLIX << "All leaders have empty recruitment lists. \n";
		return;  // This CA is going to be blacklisted for this turn.
	}

	if (current_team().gold() < optional_cheapest_unit_cost_ && optional_cheapest_unit_cost_ > 0) {
		DBG_AI_FLIX << "Not enough gold for recruiting \n";
		return;  // This CA is going to be blacklisted for this turn.
	}



	/**
	 * Step 1: Find important hexes and calculate other static things.
	 * Maybe cache it for later use.
	 * (TODO)
	 */

	/**
	 * Step 2: Filter own_recruits according to configurations
	 * (TODO)
	 */


	/**
	 * Step 3: Fill scores with values coming from combat analysis and other stuff.
	 */
	update_important_hexes();
//	if (game_config::debug) {
//		show_important_hexes();
//	}

	// For now just fill the scores with dummy entries.
	// The second and third unit get a score of 1000, all others 0
	BOOST_FOREACH(data& data, leader_data) {
		int idx = 0;
		BOOST_FOREACH(score_map::value_type& entry, data.scores) {
			const std::string& recruit = entry.first;

			data.scores[recruit] = (idx > 0 && idx < 3) ? 1000 : 0;
			++idx;
		}
		LOG_AI_FLIX << "\n" << data.to_string();
	}

	/**
	 * Step 4: Do recruitment according to scores and the leaders ratio_scores.
	 * Note that the scores don't indicate the preferred mix to recruit but rather
	 * the preferred mix of all units. So already existing units are considered.
	 */

	// Count all own units which are already on the map
	std::map<std::string, int> own_units_count;
	int total_own_units = 0;

	BOOST_FOREACH(const unit& unit, units) {
		if (unit.side() != get_side() || unit.can_recruit()) {
			continue;
		}

		++own_units_count[unit.type_name()];
		++total_own_units;
	}


	// the ratio_scores are there to decide which leader should recruit
	// normalize them.
	double ratio_score_sum = 0.0;
	BOOST_FOREACH(const data& data, leader_data) {
		ratio_score_sum += data.ratio_score;
	}
	assert(ratio_score_sum > 0.0);

	BOOST_FOREACH(data& data, leader_data) {
		data.ratio_score /= ratio_score_sum;
	}


	int total_recruit_count = 0;
	recruit_result_ptr recruit_result;
	do {
		// find which leader should recruit according to ratio_scores
		data* best_leader_data = NULL;
		double biggest_difference = -100;
		BOOST_FOREACH(data& data, leader_data) {
			double desired_percentage = data.ratio_score * 100;
			double current_percentage = (total_recruit_count == 0) ? 0 :
					(static_cast<double>(data.recruit_count) / total_recruit_count) * 100;
			double difference = desired_percentage - current_percentage;
			if (difference > biggest_difference) {
				biggest_difference = difference;
				best_leader_data = &data;
			}
		}
		assert(best_leader_data);

		// find which unit should this leader recruit according to best_leader_data.scores
		std::string best_recruit;
		biggest_difference = -100;
		BOOST_FOREACH(const score_map::value_type& i, best_leader_data->get_normalized_scores()) {
			const std::string& unit = i.first;
			const double score = i.second;

			double desired_percentage = score * 100;
			double current_percentage = (total_own_units == 0) ? 0 :
					(static_cast<double>(own_units_count[unit]) / total_own_units) * 100;
			double difference = desired_percentage - current_percentage;
			if (difference > biggest_difference) {
				biggest_difference = difference;
				best_recruit = unit;
			}
		}

		// TODO(flix): find the best hex for recruiting best_recruit.
		// see http://forums.wesnoth.org/viewtopic.php?f=8&t=36571&p=526035#p525946
		// "It also means there is a tendency to recruit from the outside in
		// rather than the default inside out."
		recruit_result = check_recruit_action(best_recruit,
				map_location::null_location,
				best_leader_data->leader->get_location());
		if (recruit_result->is_ok()) {
			recruit_result->execute();
			LOG_AI_FLIX << "Recruited " << best_recruit << "\n";
			++own_units_count[best_recruit];
			++total_own_units;
			++best_leader_data->recruit_count;
			++total_recruit_count;
			// TODO(flix): here something is needed to check if something changed in WML
			// if yes just return. evaluate() and execute() will be called again.
		} else {
			// TODO(flix): here something is needed to decide if we may want to recruit a
			// cheaper unit, when recruitment failed because of unit costs.
		}
	}while(recruit_result->is_ok());
}

recruitment::~recruitment() { }

// This is going to be called when the recruitment list changes
// (not implemented yet, just here as a reminder)
void recruitment::invalidate() {
	optional_cheapest_unit_cost_ = boost::none;
}

void recruitment::update_important_hexes() {
	important_hexes_.clear();
	pathfind::full_cost_map cost_map(MAP_IGNORE_ZOC, true, current_team(), true, false);
	add_side_to_cost_map(get_side(), &cost_map);
}

void recruitment::add_side_to_cost_map(int side, pathfind::full_cost_map* cost_map) {
	const unit_map& units = *resources::units;
	const gamemap& map = *resources::game_map;
	const team& team = (*resources::teams)[side - 1];

	// First add all existing units to cost_map.
	int unit_count = 0;
	BOOST_FOREACH(const unit& unit, units) {
		if (unit.side() != side) {
			continue;
		}
		++unit_count;
		cost_map->add_unit(unit);
	}

	// If this side has not so many units yet, add unit_types with the leaders position as origin.
	if (unit_count < MAP_UNIT_THRESHOLD) {
		std::vector<unit_map::const_iterator> leaders = units.find_leaders(side);
		BOOST_FOREACH(const unit_map::const_iterator& leader, leaders) {
			// Yes, multiple leader support for enemies too.

			// First add team-recruits (it's fine when (team-)recruits are added multiple times).
			BOOST_FOREACH(const std::string& recruit, team.recruits()) {
				cost_map->add_unit(leader->get_location(), unit_types.find(recruit), side);
			}

			// Next add extra-recruits.
			BOOST_FOREACH(const std::string& recruit, leader->recruits()) {
				cost_map->add_unit(leader->get_location(), unit_types.find(recruit), side);
			}
		}
	}

	for(int x = 0; x < map.w(); ++x) {
		for (int y = 0; y < map.h(); ++y) {
			std::stringstream s;
			s << cost_map->get_pair_at(x, y).first << " / " <<
					cost_map->get_pair_at(x, y).second << "\n\n";
			resources::screen->labels().set_label(map_location(x, y), s.str());
		}
	}
}

void recruitment::show_important_hexes() const {
	if (!game_config::debug) {
		return;
	}
	resources::screen->labels().clear_all();
	BOOST_FOREACH(const map_location& loc, important_hexes_) {
		resources::screen->labels().set_label(loc, "\n\n\u2B24");
	}
}
}  // namespace flix_recruitment
}  // namespace ai
