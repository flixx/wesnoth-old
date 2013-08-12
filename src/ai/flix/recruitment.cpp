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
#include "../manager.hpp"
#include "../../actions/attack.hpp"
#include "../../attack_prediction.hpp"
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
#include "../../util.hpp"

#include <boost/foreach.hpp>
#include <boost/scoped_ptr.hpp>
#include <math.h>

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

// When a team has less then this much units, consider recruit-list too.
const static int MAP_UNIT_THRESHOLD = 5;

// Defines the shape of the border-zone between enemies.
// Higher values mean more important hexes.
const static double MAP_BORDER_THICKNESS = 2.0;
const static double MAP_BORDER_WIDTH = 0.2;

// This parameter can be used to shift all important hexes in one directon.
// For example if our AI should act rather defensivly we may want to set
// this value to a negative number. Then the AI will more care about hexes
// nearer to the own units.
const static int MAP_OFFENSIVE_SHIFT = 0;

// When villages are this near to imprtant hexes they count as important.
const static int MAP_VILLAGE_NEARNESS_THRESHOLD = 3;

// Radius of area around important villages.
const static int MAP_VILLAGE_SURROUNDING = 1;

// Determines the power of a raw unit comparison
// A higher power means that *very good* units will be
// stronger favored compared to just *good* units.
const static double COMBAT_SCORE_POWER = 1.;

// Determines a kind of *lower threshold* for combat scores.
// A smaller value means that combat analysis will give more 0 scores.
// 0 means that only the best unit gets a 100 score and all other a 0 score.
// 1 means that all units which are worse than average will get a 0 score.
// Formula: zero_threshold = max_score - (COMBAT_SCORE_THRESHOLD * (max_score - average_score));
const static double COMBAT_SCORE_THRESHOLD = 1.;

// If set to true combat analysis will work as follows:
// For each enemy unit determine what unit would be best against it.
// Then the scores are distributed according to the enemy-units distribution.
// (For each enemy the enemies HP are added to the score of our best unit against it)
// COMBAT_SCORE_POWER and COMBAT_SCORE_THRESHOLD are ignored then.
const static bool COMBAT_DIRECT_RESPONSE = false;

// A cache is used to store the simulation results.
// This value determines how much the average defenses of the important hexes can differ
// until the simulation will run again.
const static double COMBAT_CACHE_TOLERANCY = 0.5;

const static double MAP_ANALYSIS_WEIGHT = 0.;
const static double COMABAT_ANALYSIS_WEIGHT = 1.;

// Used for time measurements.
// REMOVE ME
static long timer_fight = 0;
static long timer_simulation = 0;
}

recruitment::recruitment(rca_context& context, const config& cfg)
		: candidate_action(context, cfg),
		  recruit_situation_change_observer_(),
		  cheapest_unit_costs_() { }

double recruitment::evaluate() {
	// Check if the recruitment list has changed.
	// Then cheapest_unit_costs_ is not valid anymore.
	if (recruit_situation_change_observer_.recruit_list_changed()) {
		invalidate();
		recruit_situation_change_observer_.set_recruit_list_changed(false);
	}
	const unit_map& units = *resources::units;
	const std::vector<unit_map::const_iterator> leaders = units.find_leaders(get_side());

	BOOST_FOREACH(const unit_map::const_iterator& leader, leaders) {
		if (leader == resources::units->end()) {
			return BAD_SCORE;
		}
		// Check Gold. But proceed if there is a unit with cost <= 0 (WML can do that)
		int cheapest_unit_cost = get_cheapest_unit_cost_for_leader(leader);
		if (current_team().gold() < cheapest_unit_cost && cheapest_unit_cost > 0) {
			return BAD_SCORE;
		}

		const map_location& loc = leader->get_location();
		if (resources::game_map->is_keep(loc) &&
				pathfind::find_vacant_castle(*leader) != map_location::null_location) {
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

	const unit_map& units = *resources::units;
	const gamemap& map = *resources::game_map;
	const std::vector<unit_map::const_iterator> leaders = units.find_leaders(get_side());

	// This is the central datastructure with all score_tables in it.
	std::vector<data> leader_data;

	std::set<std::string> global_recruits;

	BOOST_FOREACH(const unit_map::const_iterator& leader, leaders) {
		const map_location& keep = leader->get_location();
		if (!resources::game_map->is_keep(keep)) {
			DBG_AI_FLIX << "Leader " << leader->name() << " is not on keep. \n";
			continue;
		}
		if (pathfind::find_vacant_castle(*leader) == map_location::null_location) {
			DBG_AI_FLIX << "Leader " << leader->name() << " is on keep but no hexes are free \n";
			continue;
		}
		int cheapest_unit_cost = get_cheapest_unit_cost_for_leader(leader);
		if (current_team().gold() < cheapest_unit_cost && cheapest_unit_cost > 0) {
			DBG_AI_FLIX << "Leader " << leader->name() << " recruits are to expensive. \n";
			continue;
		}

		// Leader can recruit.

		data data(leader);

		// Add team recruits.
		BOOST_FOREACH(const std::string& recruit, current_team().recruits()) {
			data.recruits.insert(recruit);
			data.scores[recruit] = 0.0;
			data.limits[recruit] = 99999;
			global_recruits.insert(recruit);
		}

		// Add extra recruits.
		BOOST_FOREACH(const std::string& recruit, leader->recruits()) {
			data.recruits.insert(recruit);
			data.scores[recruit] = 0.0;
			data.limits[recruit] = 99999;
			global_recruits.insert(recruit);
		}
		leader_data.push_back(data);
	}

	if (leader_data.empty()) {
		DBG_AI_FLIX << "No leader available for recruiting. \n";
		return;  // This CA is going to be blacklisted for this turn.
	}

	if (global_recruits.empty()) {
		DBG_AI_FLIX << "All leaders have empty recruitment lists. \n";
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

	BOOST_FOREACH(const map_location& hex, important_hexes_) {
		++important_terrain_[map[hex]];
	}

	/**
	 * Step 2: Filter own_recruits according to configurations
	 * (TODO)
	 */


	/**
	 * Step 3: Fill scores with values coming from combat analysis and other stuff.
	 */

	int start =  SDL_GetTicks();
	do_map_analysis(&leader_data);
	int end =  SDL_GetTicks();
	LOG_AI_FLIX << "Map-analysis: " << static_cast<double>(end - start) / 1000 << " seconds.\n";
	start =  SDL_GetTicks();
	do_combat_analysis(&leader_data);
	end =  SDL_GetTicks();
	LOG_AI_FLIX << "Combat-analysis: " << static_cast<double>(end - start) / 1000 << " seconds.\n";
	LOG_AI_FLIX << "In fight: " << static_cast<double>(timer_fight) / 1000 << " seconds.\n";
	LOG_AI_FLIX << "In simulation: " << static_cast<double>(timer_simulation) / 1000 << " seconds.\n";
	timer_simulation = 0;
	timer_fight = 0;

	BOOST_FOREACH(const data& data, leader_data) {
		LOG_AI_FLIX << "\n" << data.to_string();
	}

	/**
	 * Step 4: Do recruitment according to scores and the leaders ratio_scores.
	 * Note that the scores don't indicate the preferred mix to recruit but rather
	 * the preferred mix of all units. So already existing units are considered.
	 */


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

		const std::string best_recruit = get_best_recruit_from_scores(*best_leader_data);

		// TODO(flix): find the best hex for recruiting best_recruit.
		// see http://forums.wesnoth.org/viewtopic.php?f=8&t=36571&p=526035#p525946
		// "It also means there is a tendency to recruit from the outside in
		// rather than the default inside out."
		recruit_result = check_recruit_action(best_recruit,
				map_location::null_location,
				best_leader_data->leader->get_location());
		if (recruit_result->is_ok()) {
			recruit_situation_change_observer_.reset_gamestate_changed();
			recruit_result->execute();
			LOG_AI_FLIX << "Recruited " << best_recruit << "\n";
			++best_leader_data->recruit_count;
			++total_recruit_count;

			// Check if something changed in the recruitment list (WML can do that).
			// If yes, just return. evaluate() and execute() will be called again.
			if (recruit_situation_change_observer_.recruit_list_changed()) {
				return;
			}
			// Check if the gamestate changed more than once.
			// (Recruitment will trigger one gamestate change, WML could trigger more changes.)
			// If yes, just return. evaluate() and execute() will be called again.
			if (recruit_situation_change_observer_.gamestate_changed() > 1) {
				return;
			}

		} else {
			LOG_AI_FLIX << "Recruit result not ok.\n";
			// TODO(flix): here something is needed to decide if we may want to recruit a
			// cheaper unit, when recruitment failed because of unit costs.
		}
	} while(recruit_result->is_ok());
}

/**
 * A helper function for execute().
 * Counts own units and then decides what unit should be recruited so that the
 * unit distribution approaches the given scores.
 */
const std::string recruitment::get_best_recruit_from_scores(const data& leader_data) const {
	const unit_map& units = *resources::units;

	// Count all own units which are already on the map
	std::map<std::string, int> own_units_count;
	int total_own_units = 0;
	BOOST_FOREACH(const unit& unit, units) {
		if (unit.side() != get_side() || unit.can_recruit()) {
			continue;
		}
		++own_units_count[unit.type_id()];
		++total_own_units;
	}

	// find which unit should this leader recruit according to best_leader_data.scores
	std::string best_recruit;
	double biggest_difference = 0;
	BOOST_FOREACH(const score_map::value_type& i, leader_data.get_normalized_scores()) {
		const std::string& unit = i.first;
		const double score = i.second;

		double desired_ammount = score * (total_own_units + 1);
		double current_ammount = own_units_count[unit];
		double difference = desired_ammount - current_ammount;
		if (difference > biggest_difference) {
			biggest_difference = difference;
			best_recruit = unit;
		}
	}
	return best_recruit;
}

recruitment::~recruitment() { }

// This is going to be called when the recruitment list changes
// (not implemented yet, just here as a reminder)
void recruitment::invalidate() {
	cheapest_unit_costs_.clear();;
}

/**
 * Called at the beginning and whenever the recruitment list changes.
 */
int recruitment::get_cheapest_unit_cost_for_leader(const unit_map::const_iterator& leader) {
	std::map<size_t, int>::const_iterator it = cheapest_unit_costs_.find(leader->underlying_id());
	if (it != cheapest_unit_costs_.end()) {
		return it->second;
	}

	int cheapest_cost = 999999;

	// team recruits
	BOOST_FOREACH(const std::string& recruit, current_team().recruits()) {
		const unit_type* const info = unit_types.find(recruit);
		if (info->cost() < cheapest_cost) {
			cheapest_cost = info->cost();
		}
	}
	// extra recruits
	BOOST_FOREACH(const std::string& recruit, leader->recruits()) {
		const unit_type* const info = unit_types.find(recruit);
		if (info->cost() < cheapest_cost) {
			cheapest_cost = info->cost();
		}
	}
	LOG_AI_FLIX << "Cheapest unit cost updated to " << cheapest_cost << ".\n";
	cheapest_unit_costs_[leader->underlying_id()] = cheapest_cost;
	return cheapest_cost;
}

/**
 * For Map Analysis.
 * Creates a std::set of hexes where a fight will occur with high probability.
 */
void recruitment::update_important_hexes() {
	important_hexes_.clear();
	important_terrain_.clear();
	update_average_local_cost();
	const gamemap& map = *resources::game_map;
	const unit_map& units = *resources::units;

	// TODO(flix) If leader is in danger or only leader is left
	// mark only hexes near to this leader as important.

	// Mark battle areas as important
	// This are locations where one of my units is adjacent
	// to a enemies unit.
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
	// This is done by creating a cost_map for each team.
	// A cost_map maps to each hex the average costs to reach this hex
	// for all units of the team.
	// The important hexes are those where my value on the cost map is
	// similar to a enemies one.
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

/**
 * For Map Analysis.
 * Creates cost maps for a side. Each hex is map to
 * a) the summed movecost and
 * b) how many units can reach this hex
 * for all units of side.
 */
const  pathfind::full_cost_map recruitment::get_cost_map_of_side(int side) const {
	const unit_map& units = *resources::units;
	const team& team = (*resources::teams)[side - 1];

	pathfind::full_cost_map cost_map(true, true, team, true, true);

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

/**
 * For Map Analysis
 * Computes from our cost map and the combined cost map of all enemies the important hexes.
 */
void recruitment::compare_cost_maps_and_update_important_hexes(
		const pathfind::full_cost_map& my_cost_map,
		const pathfind::full_cost_map& enemy_cost_map) {

	const gamemap& map = *resources::game_map;

	// First collect all hexes where the average costs are similar in important_hexes_candidates
	// Then chose only those hexes where the average costs are relatively low.
	// This is done to remove hexes to where the teams need a similar amount of moves but
	// which are relatively far away comparing to other important hexes.
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
			// We multiply the threshold MAP_BORDER_THICKNESS by the average_local_cost
			// to favor high cost hexes (a bit).
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

/**
 * For Map Analysis.
 * Shows the important hexes for debugging purposes on the map. Only if debug is activated.
 */
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

/**
 * For Map Analysis.
 * Creates a map where each hex is mapped to the average cost of the terrain for our units.
 */
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

/**
 * Map Analysis.
 * When this function is called, important_hexes_ is already build.
 * This function fills the scores according to important_hexes_.
 */
void recruitment::do_map_analysis(std::vector<data>* leader_data) {
	BOOST_FOREACH(data& data, *leader_data) {
		BOOST_FOREACH(const std::string& recruit, data.recruits) {
			data.scores[recruit] += get_average_defense(recruit) * MAP_ANALYSIS_WEIGHT;
		}
	}
}

/**
 * For Map Analysis.
 * Calculates for a given unit the average defense on the map.
 * (According to important_hexes_ / important_terrain_)
 */
double recruitment::get_average_defense(const std::string& u_type) const {
	const unit_type* const u_info = unit_types.find(u_type);
	if (!u_info) {
		return 0.0;
	}
	long summed_defense = 0;
	int total_terrains = 0;
	BOOST_FOREACH(const terrain_count_map::value_type& entry, important_terrain_) {
		const t_translation::t_terrain& terrain = entry.first;
		int count = entry.second;
		int defense = 100 - u_info->movement_type().defense_modifier(terrain);
		summed_defense += defense * count;
		total_terrains += count;
	}
	double average_defense = (total_terrains == 0) ? 0.0 :
			static_cast<double>(summed_defense) / total_terrains;
	return average_defense;
}

/**
 * Combat Analysis.
 * Main function.
 * Compares all enemy units with all of our possible recruits and fills
 * the scores.
 */
void recruitment::do_combat_analysis(std::vector<data>* leader_data) {

	const unit_map& units = *resources::units;

	BOOST_FOREACH(data& leader, *leader_data) {
		if (leader.recruits.empty()) {
			continue;
		}
		typedef std::map<std::string, double> simple_score_map;
		simple_score_map temp_scores;

		BOOST_FOREACH(const unit& unit, units) {
			if (!current_team().is_enemy(unit.side())) {
				continue;
			}
			std::string best_response;
			double best_response_score = -99999.;
			BOOST_FOREACH(const std::string& recruit, leader.recruits) {
				double score = compare_unit_types(recruit, unit.type_id());
				score *= unit.hitpoints();
				score = pow(score, COMBAT_SCORE_POWER);
				temp_scores[recruit] += (COMBAT_DIRECT_RESPONSE) ? 0 : score;
				if (score > best_response_score) {
					best_response_score = score;
					best_response = recruit;
				}
			}
			if (COMBAT_DIRECT_RESPONSE) {
				temp_scores[best_response] += unit.hitpoints();
			}
		}

		// Find things for normalization.
		double max = -99999.;
		double sum = 0;
		BOOST_FOREACH(const simple_score_map::value_type& entry, temp_scores) {
			double score = entry.second;
			if (score > max) {
				max = score;
			}
			sum += score;
		}
		assert(!temp_scores.empty());
		double average = sum / temp_scores.size();

		// What we do now is a linear transformation.
		// We want to map the scores in temp_scores to something between 0 and 100.
		// The max score shall always be 100.
		// The min score depends on parameters.
		double new_100 = max;
		double score_threshold = (COMBAT_SCORE_THRESHOLD > 0) ? COMBAT_SCORE_THRESHOLD : 0.000001;
		double new_0 = (COMBAT_DIRECT_RESPONSE) ? 0 : max - (score_threshold * (max - average));
		if (new_100 == new_0) {
			// This can happen if max == average. (E.g. only one possible recruit)
			new_0 -= 0.000001;
		}

		BOOST_FOREACH(const simple_score_map::value_type& entry, temp_scores) {
			const std::string& recruit = entry.first;
			double score = entry.second;

			// Here we transform.
			// (If score <= new_0 then normalized_score will be 0)
			// (If score = new_100 then normalized_score will be 100)
			double normalized_score = 100 * ((score - new_0) / (new_100 - new_0));
			if (normalized_score < 0) {
				normalized_score = 0;
			}
			leader.scores[recruit] += normalized_score * COMABAT_ANALYSIS_WEIGHT;
		}
	}  // for all leaders
}

/**
 * For Combat Analysis.
 * Calculates how good unit-type a is against unit type b.
 * If the value is bigger then 1, a is better then b.
 * The value is always bigger then 0.
 * If the value is 2 then unit-type a is twice as good as unit-type b.
 * Since this function is called very often it uses a cache.
 */
double recruitment::compare_unit_types(const std::string& a, const std::string& b) {
	const unit_type* const type_a = unit_types.find(a);
	const unit_type* const type_b = unit_types.find(b);
	if (!type_a || !type_b) {
		ERR_AI_FLIX << "Couldn't find unit type: " << ((type_a) ? b : a) << ".\n";
		return 0.0;
	}
	double defense_a = get_average_defense(a);
	double defense_b = get_average_defense(b);

	const double* cache_value = get_cached_combat_value(a, b, defense_a, defense_b);
	if (cache_value) {
		return *cache_value;
	}

	double damage_to_a = 0.0;
	double damage_to_b = 0.0;

	// a attacks b
	simulate_attack(type_a, type_b, defense_a, defense_b, &damage_to_a, &damage_to_b);
	// b attacks a
	simulate_attack(type_b, type_a, defense_b, defense_a, &damage_to_b, &damage_to_a);

	int a_cost = (type_a->cost() > 0) ? type_a->cost() : 1;
	int b_cost = (type_b->cost() > 0) ? type_b->cost() : 1;
	int a_max_hp = (type_a->hitpoints() > 0) ? type_a->hitpoints() : 1;
	int b_max_hp = (type_b->hitpoints() > 0) ? type_b->hitpoints() : 1;

	double retval = 1.;
	// There are rare cases where a unit deals 0 damage (eg. Elvish Lady).
	// Then we just set the value to something reasonable.
	if (damage_to_a <= 0 && damage_to_b <= 0) {
		retval = 1.;
	} else if (damage_to_a <= 0) {
		retval = 2.;
	} else if (damage_to_b <= 0) {
		retval = 0.5;
	} else {
		// Normal case
		double value_of_a = damage_to_b / (b_max_hp * a_cost);
		double value_of_b = damage_to_a / (a_max_hp * b_cost);

		if (value_of_a > value_of_b) {
			return value_of_a / value_of_b;
		} else if (value_of_a < value_of_b) {
			return -value_of_b / value_of_a;
		} else {
			return 0.;
		}
	}

	// Insert in cache.
	const cached_combat_value entry(defense_a, defense_b, retval);
	std::set<cached_combat_value>& cache = combat_cache[a][b];
	cache.insert(entry);

	return retval;
}

/**
 * For Combat Analysis.
 * Returns the cached combat value for two unit types
 * or NULL if there is none or terrain defenses are not within range.
 */
const double* recruitment::get_cached_combat_value(const std::string& a, const std::string& b,
		double a_defense, double b_defense) {
	double best_distance = 999;
	const double* best_value = NULL;
	const std::set<cached_combat_value>& cache = combat_cache[a][b];
	BOOST_FOREACH(const cached_combat_value& entry, cache) {
		double distance_a = std::abs(entry.a_defense - a_defense);
		double distance_b = std::abs(entry.b_defense - b_defense);
		if (distance_a <= COMBAT_CACHE_TOLERANCY && distance_b <= COMBAT_CACHE_TOLERANCY) {
			if(distance_a + distance_b <= best_distance) {
				best_distance = distance_a + distance_b;
				best_value = &entry.value;
			}
		}
	}
	return best_value;
}

/**
 * For Combat Analysis.
 * This struct encapsulates all information for one attack simulation.
 * One attack simulation is defined by the unit-types, the weapons and the units defenses.
 */
struct attack_simulation {
	const unit_type* attacker_type;
	const unit_type* defender_type;
	const battle_context_unit_stats attacker_stats;
	const battle_context_unit_stats defender_stats;
	combatant attacker_combatant;
	combatant defender_combatant;

	attack_simulation(const unit_type* attacker, const unit_type* defender,
			double attacker_defense, double defender_defense,
			const attack_type* att_weapon, const attack_type* def_weapon) :
			attacker_type(attacker),
			defender_type(defender),
			attacker_stats(attacker, att_weapon, true, defender, def_weapon,
					round_double(defender_defense), 0),
			defender_stats(defender, def_weapon, false, attacker, att_weapon,
					round_double(attacker_defense), 0),
			attacker_combatant(attacker_stats),
			defender_combatant(defender_stats)
	{
		int start = SDL_GetTicks();  // REMOVE ME
		attacker_combatant.fight(defender_combatant);
		timer_fight += SDL_GetTicks() - start;  // REMOVE ME
	}

	bool better_result(const attack_simulation* other, bool for_defender) {
		assert(other);
		if (for_defender) {
			return battle_context::better_combat(
					defender_combatant, attacker_combatant,
					other->defender_combatant, other->attacker_combatant, 0);
		} else {
			return battle_context::better_combat(
					attacker_combatant, defender_combatant,
					other->attacker_combatant, other->defender_combatant, 0);
		}
	}

	double get_avg_hp_of_defender() const {
		return get_avg_hp_of_combatant(false);
	}

	double get_avg_hp_of_attacker() const {
		return get_avg_hp_of_combatant(true);
	}
	double get_avg_hp_of_combatant(bool attacker) const {
		const combatant& combatant = (attacker) ? attacker_combatant : defender_combatant;
		const unit_type* unit_type = (attacker) ? attacker_type : defender_type;
		double avg_hp = combatant.average_hp(0);

		// handle poisson
		avg_hp -= combatant.poisoned * game_config::poison_amount;

		// handle regenaration
		// (test shown that it is better not to do this here)
		// REMOVE ME
//		unit_ability_list regen_list;
//		if (const config &abilities = unit_type->get_cfg().child("abilities")) {
//			BOOST_FOREACH(const config &i, abilities.child_range("regenerate")) {
//				regen_list.push_back(unit_ability(&i, map_location::null_location));
//			}
//		}
//		unit_abilities::effect regen_effect(regen_list, 0, false);
//		avg_hp += regen_effect.get_composite_value();

		avg_hp = std::max(0., avg_hp);
		avg_hp = std::min(static_cast<double>(unit_type->hitpoints()), avg_hp);
		return avg_hp;
	}
};

/**
 * For Combat Analysis.
 * Simulates a attack with a attacker and a defender.
 * The function will use battle_context::better_combat() to decide which weapon to use.
 */
void recruitment::simulate_attack(
			const unit_type* const attacker, const unit_type* const defender,
			double attacker_defense, double defender_defense,
			double* damage_to_attacker, double* damage_to_defender) const {
	if(!attacker || !defender || !damage_to_attacker || !damage_to_defender) {
		ERR_AI_FLIX << "NULL pointer in simulate_attack()\n";
		return;
	}
	const std::vector<attack_type> attacker_weapons = attacker->attacks();
	const std::vector<attack_type> defender_weapons = defender->attacks();

	boost::shared_ptr<attack_simulation> best_att_attack;

	// Let attacker choose weapon
	BOOST_FOREACH(const attack_type& att_weapon, attacker_weapons) {
		boost::shared_ptr<attack_simulation> best_def_response;
		// Let defender choose weapon
		BOOST_FOREACH(const attack_type& def_weapon, defender_weapons) {
			if (att_weapon.range() != def_weapon.range()) {
				continue;
			}
			int start = SDL_GetTicks();  // REMOVE ME
			boost::shared_ptr<attack_simulation> simulation(new attack_simulation(
					attacker, defender,
					attacker_defense, defender_defense,
					&att_weapon, &def_weapon));
			timer_simulation += SDL_GetTicks() - start;  // REMOVE ME
			if (!best_def_response || simulation->better_result(best_def_response.get(), true)) {
				best_def_response = simulation;
			}
		}  // for defender weapons

		if (!best_def_response) {
			// Defender can not fight back. Simulate this as well.
			best_def_response.reset(new attack_simulation(
					attacker, defender,
					attacker_defense, defender_defense,
					&att_weapon, NULL));
		}
		if (!best_att_attack || best_def_response->better_result(best_att_attack.get(), false)) {
			best_att_attack = best_def_response;
		}
	}  // for attacker weapons

	if (!best_att_attack) {
		return;
	}

	*damage_to_defender += (defender->hitpoints() - best_att_attack->get_avg_hp_of_defender());
	*damage_to_attacker += (attacker->hitpoints() - best_att_attack->get_avg_hp_of_attacker());
}

/**
 * Observer Code
 */
recruitment::recruit_situation_change_observer::recruit_situation_change_observer()
	: recruit_list_changed_(false), gamestate_changed_(0) {
	manager::add_recruit_list_changed_observer(this);
	manager::add_gamestate_observer(this);
}

void recruitment::recruit_situation_change_observer::handle_generic_event(
		const std::string& event) {
	if (event == "ai_recruit_list_changed") {
		LOG_AI_FLIX << "Recruitment List is not valid anymore.\n";
		set_recruit_list_changed(true);
	} else {
		LOG_AI_FLIX << "Event is " << event << ". -> Gamestate changed.\n";
		++gamestate_changed_;
	}
}

recruitment::recruit_situation_change_observer::~recruit_situation_change_observer() {
	manager::remove_recruit_list_changed_observer(this);
	manager::remove_gamestate_observer(this);
}

bool recruitment::recruit_situation_change_observer::recruit_list_changed() {
	return recruit_list_changed_;
}

void recruitment::recruit_situation_change_observer::set_recruit_list_changed(bool changed) {
	recruit_list_changed_ = changed;
}

int recruitment::recruit_situation_change_observer::gamestate_changed() {
	return gamestate_changed_;
}

void recruitment::recruit_situation_change_observer::reset_gamestate_changed() {
	gamestate_changed_ = 0;
}
}  // namespace flix_recruitment
}  // namespace ai
