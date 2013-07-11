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
#include "../../pathutils.hpp"
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
const static double MAP_BORDER_THICKNESS = 2.0;
const static double MAP_BORDER_WIDTH = 0.2;
const static int MAP_VILLAGE_NEARNESS_THRESHOLD = 3;
const static int MAP_VILLAGE_SURROUNDING = 1;
const static int MAP_OFFENSIVE_SHIFT = 0;

const static double MAP_ANALYSIS_WEIGHT = 1.;
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
	const gamemap& map = *resources::game_map;
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

	update_important_hexes();
	if (game_config::debug) {
		show_important_hexes();
	}

	terrain_count_map important_terrain;
	BOOST_FOREACH(const map_location& hex, important_hexes_) {
		++important_terrain[map[hex]];
	}

	/**
	 * Step 2: Filter own_recruits according to configurations
	 * (TODO)
	 */


	/**
	 * Step 3: Fill scores with values coming from combat analysis and other stuff.
	 */

	do_map_analysis(important_terrain, &leader_data);

	BOOST_FOREACH(const data& data, leader_data) {
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
	update_average_local_cost();
	const gamemap& map = *resources::game_map;
	const unit_map& units = *resources::units;

	// TODO(flix) If leader is in danger or only leader is left
	// mark only hexes near to this leader as important.

	// Mark battle areas as important
	BOOST_FOREACH(const unit& unit, units) {
		if (unit.side() != get_side()) {
			continue;
		}
		std::vector<map_location> surrounding;
		get_tiles_in_radius(unit.get_location(), MAP_VILLAGE_SURROUNDING, surrounding);
		if (surrounding.empty()) {
			continue;
		}
		BOOST_FOREACH(const map_location& loc, surrounding) {
			const unit_map::const_iterator& enemy_it = units.find(loc);
			if(enemy_it == units.end()) {
				continue;
			}
			if (!current_team().is_enemy(enemy_it->side())) {
				continue;
			}
			// We found a enemy next to us. Mark our unit and all adjacent
			// hexes as important.
			important_hexes_.insert(unit.get_location());
			std::copy(surrounding.begin(), surrounding.end(),
					std::inserter(important_hexes_, important_hexes_.begin()));
		}
	}

	// Mark area between me and enemies as important
	const pathfind::full_cost_map my_cost_map = get_cost_map_of_side(get_side());
	BOOST_FOREACH(const team& team, *resources::teams) {
		if (current_team().is_enemy(team.side())) {
			const pathfind::full_cost_map enemy_cost_map = get_cost_map_of_side(team.side());

			compare_cost_maps_and_update_important_hexes(my_cost_map, enemy_cost_map);
		}
	}

	// Mark 'near' villages and area around them as important
	// To prevent a 'feedback' of important locations collect all
	// important villages first and add them and their surroundings
	// to important_hexes_ in a second step.
	std::vector<map_location> important_villages;
	BOOST_FOREACH(const map_location& village, map.villages()) {
		std::vector<map_location> surrounding;
		get_tiles_in_radius(village, MAP_VILLAGE_NEARNESS_THRESHOLD, surrounding);
		BOOST_FOREACH(const map_location& hex, surrounding) {
			if (important_hexes_.find(hex) != important_hexes_.end()) {
				important_villages.push_back(village);
				break;
			}
		}
	}
	BOOST_FOREACH(const map_location& village, important_villages) {
		important_hexes_.insert(village);
		std::vector<map_location> surrounding;
		get_tiles_in_radius(village, MAP_VILLAGE_SURROUNDING, surrounding);
		BOOST_FOREACH(const map_location& hex, surrounding) {
			// only add hex if one of our units can reach the hex
			if (map.on_board(hex) && my_cost_map.get_cost_at(hex.x, hex.y) != -1) {
				important_hexes_.insert(hex);
			}
		}
	}
}

const  pathfind::full_cost_map recruitment::get_cost_map_of_side(int side) const {
	const unit_map& units = *resources::units;
	const team& team = (*resources::teams)[side - 1];

	pathfind::full_cost_map cost_map(MAP_IGNORE_ZOC, true, team, true, true);

	// First add all existing units to cost_map.
	int unit_count = 0;
	BOOST_FOREACH(const unit& unit, units) {
		if (unit.side() != side || unit.can_recruit()) {
			continue;
		}
		++unit_count;
		cost_map.add_unit(unit);
	}

	// If this side has not so many units yet, add unit_types with the leaders position as origin.
	if (unit_count < MAP_UNIT_THRESHOLD) {
		std::vector<unit_map::const_iterator> leaders = units.find_leaders(side);
		BOOST_FOREACH(const unit_map::const_iterator& leader, leaders) {
			// Yes, multiple leader support for enemies too.

			// First add team-recruits (it's fine when (team-)recruits are added multiple times).
			BOOST_FOREACH(const std::string& recruit, team.recruits()) {
				cost_map.add_unit(leader->get_location(), unit_types.find(recruit), side);
			}

			// Next add extra-recruits.
			BOOST_FOREACH(const std::string& recruit, leader->recruits()) {
				cost_map.add_unit(leader->get_location(), unit_types.find(recruit), side);
			}
		}
	}
	return cost_map;
}

void recruitment::compare_cost_maps_and_update_important_hexes(
		const pathfind::full_cost_map& my_cost_map,
		const pathfind::full_cost_map& enemy_cost_map) {

	const gamemap& map = *resources::game_map;

	// First collect all hexes where the average costs are similar in important_hexes_candidates
	// Then chose only those hexes where the average costs are relatively low.
	typedef std::map<map_location, double> border_cost_map;
	border_cost_map important_hexes_candidates;
	double smallest_border_movecost = 999999;
	double biggest_border_movecost = 0;
	for(int x = 0; x < map.w(); ++x) {
		for (int y = 0; y < map.h(); ++y) {
			double my_cost_average = my_cost_map.get_average_cost_at(x, y);
			double enemy_cost_average = enemy_cost_map.get_average_cost_at(x, y);
			if (my_cost_average == -1 || enemy_cost_average == -1) {
				continue;
			}
			if (std::abs(my_cost_average - MAP_OFFENSIVE_SHIFT - enemy_cost_average) <
					MAP_BORDER_THICKNESS * average_local_cost_[map_location(x, y)]) {
				double border_movecost = (my_cost_average + enemy_cost_average) / 2;

				important_hexes_candidates[map_location(x, y)] = border_movecost;

				if (border_movecost < smallest_border_movecost) {
					smallest_border_movecost = border_movecost;
				}
				if (border_movecost > biggest_border_movecost) {
					biggest_border_movecost = border_movecost;
				}
			}
		}  // for
	}  // for
	double threshold = (biggest_border_movecost - smallest_border_movecost) *
			MAP_BORDER_WIDTH + smallest_border_movecost;
	BOOST_FOREACH(const border_cost_map::value_type& candidate, important_hexes_candidates) {
		if (candidate.second < threshold) {
			important_hexes_.insert(candidate.first);
		}
	}
}

void recruitment::show_important_hexes() const {
	if (!game_config::debug) {
		return;
	}
	resources::screen->labels().clear_all();
	BOOST_FOREACH(const map_location& loc, important_hexes_) {
		// Little hack: use map_location north from loc and make 2 linebreaks to center the dot
		resources::screen->labels().set_label(loc.get_direction(map_location::NORTH), "\n\n\u2B24");
	}
}

void recruitment::update_average_local_cost() {
	average_local_cost_.clear();
	const gamemap& map = *resources::game_map;
	const team& team = (*resources::teams)[get_side() - 1];

	for(int x = 0; x < map.w(); ++x) {
		for (int y = 0; y < map.h(); ++y) {
			map_location loc(x, y);
			int summed_cost = 0;
			int count = 0;
			BOOST_FOREACH(const std::string& recruit, team.recruits()){
				const unit_type* const unit_type = unit_types.find(recruit);
				int cost = unit_type->movement_type().get_movement().cost(map[loc]);
				if (cost < 99) {
					summed_cost += cost;
					++count;
				}
			}
			average_local_cost_[loc] = (count == 0) ? 0 : static_cast<double>(summed_cost) / count;
		}
	}
}

void recruitment::do_map_analysis(
		const terrain_count_map& important_terrain,
		std::vector<data>* leader_data) {
	BOOST_FOREACH(data& data, *leader_data) {
		BOOST_FOREACH(const std::string& recruit, data.recruits) {
			const unit_type* const unit_type = unit_types.find(recruit);
			long summed_defense = 0;
			int total_terrains = 0;
			BOOST_FOREACH(const terrain_count_map::value_type& entry, important_terrain) {
				const t_translation::t_terrain& terrain = entry.first;
				int count = entry.second;
				int defense = 100 - unit_type->movement_type().defense_modifier(terrain);
				summed_defense += defense * count;
				total_terrains += count;
			}
			double average_defense = (total_terrains == 0) ? 0 :
					static_cast<double>(summed_defense) / total_terrains;
			data.scores[recruit] += average_defense * MAP_ANALYSIS_WEIGHT;
		}
	}
}
}  // namespace flix_recruitment
}  // namespace ai
