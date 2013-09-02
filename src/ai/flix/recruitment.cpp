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
#include "../../tod_manager.hpp"
#include "../../unit_map.hpp"
#include "../../unit_types.hpp"
#include "../../util.hpp"
#include "../../variable.hpp"

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

// When a enemy is in this radius around a leader, this leader is tagged as 'in danger'.
// If gold is available, this leader will recruit as much units as possible.
const static int LEADER_IN_DANGER_RADIUS = 3;

// Save Gold Strategies will work as follow:
// The AI will always keep track of the ratio
// our_total_unit_costs / enemy_total_unit_costs
// whereas the costs are the sum of the cost of all units on the map weighted by their HP.
// When this ratio is bigger then SAVE_GOLD_BEGIN_THRESHOLD, the AI will stop recruiting units
// until the ratio is less then SAVE_GOLD_END_THRESHOLD.
const static bool ACTIVATE_SAVE_GOLD_STRATEGIES = false;
const static double SAVE_GOLD_BEGIN_THRESHOLD = 1.0;
const static double SAVE_GOLD_END_THRESHOLD = 0.7;

// When we have earned this much gold, the AI will start spending all gold to start
// a big offensive wave.
const static int SPEND_ALL_GOLD_GOLD_THRESHOLD = -1;

// This is used for a income estimation. We'll calculate the estimated income of this much
// future turns and decide if we'd gain gold if we start to recruit no units anymore.
const static int SAVE_GOLD_FORECAST_TURNS = 5;

// When a team has less then this much units, consider recruit-list too.
const static unsigned int UNIT_THRESHOLD = 5;

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
const static double COMBAT_SCORE_THRESHOLD = 1.5;

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
const static double DIVERSITY_WEIGHT = 0.5;

const static double RANDOMNESS_MAXIMUM = 15.;

// Used for time measurements.
// REMOVE ME
static long timer_fight = 0;
static long timer_simulation = 0;
}

recruitment::recruitment(rca_context& context, const config& cfg)
		: candidate_action(context, cfg),
		  own_units_in_combat_counter_(0),
		  cheapest_unit_costs_(),
		  state_(NORMAL),
		  recruit_situation_change_observer_(),
		  average_lawful_bonus_(0.0),
		  recruitment_instructions_(),
		  recruitment_instructions_turn_(-1),
		  own_units_count_(),
		  total_own_units_(0) { }

double recruitment::evaluate() {
	// Check if the recruitment list has changed.
	// Then cheapest_unit_costs_ is not valid anymore.
	if (recruit_situation_change_observer_.recruit_list_changed()) {
		invalidate();
		recruit_situation_change_observer_.set_recruit_list_changed(false);
	}

	// When evaluate() is called the first time this turn,
	// we'll retrieve the recruitment-instruction aspect.
	if (resources::tod_manager->turn() != recruitment_instructions_turn_) {
		recruitment_instructions_ = get_recruitment_instructions();
		recruitment_instructions_turn_ = resources::tod_manager->turn();
		LOG_AI_FLIX << "Recruitment-instructions updated:\n" << recruitment_instructions_ << "\n";
	}

	// Check if we have something to do.
	const config* job = get_most_important_job();
	if (!job) {
		return BAD_SCORE;
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
			continue;
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
	LOG_AI_FLIX << "\n\n\n------------FLIX RECRUITMENT BEGIN ------------\n\n";
	LOG_AI_FLIX << "TURN: " << resources::tod_manager->turn() <<
			" SIDE: " << current_team().side() << "\n";

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
			global_recruits.insert(recruit);
		}

		// Add extra recruits.
		BOOST_FOREACH(const std::string& recruit, leader->recruits()) {
			data.recruits.insert(recruit);
			data.scores[recruit] = 0.0;
			global_recruits.insert(recruit);
		}

		// Add recalls.
		// Recalls are treated as recruits. While recruiting
		// we'll check if we can do a recall instead of a recruitment.
		BOOST_FOREACH(const unit& recall, current_team().recall_list()) {
			// Check if this leader is allowed to recall this unit.
			vconfig filter = vconfig(leader->recall_filter());
			if (!recall.matches_filter(filter, map_location::null_location)) {
				continue;
			}
			data.recruits.insert(recall.type_id());
			data.scores[recall.type_id()] = 0.0;
			global_recruits.insert(recall.type_id());
		}

		// Check if leader is in danger.
		data.in_danger = is_enemy_in_radius(leader->get_location(), LEADER_IN_DANGER_RADIUS);
		// If yes, set ratio_score very high, so this leader will get priority while recruiting.
		if (data.in_danger) {
			data.ratio_score = 50;
			state_ = LEADER_IN_DANGER;
			LOG_AI_FLIX << "Leader " << leader->name() << " is in danger.\n";
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

	update_average_lawful_bonus();
	update_own_units_count();

	/**
	 * Step 2: Filter own_recruits according to configurations
	 * (TODO)
	 */

	LOG_AI_FLIX << "RECRUITMENT INSTRUCTIONS:\n" << recruitment_instructions_ << "\n";

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
	LOG_AI_FLIX << "Scores before extra treatments:\n";
	BOOST_FOREACH(const data& data, leader_data) {
		LOG_AI_FLIX << "\n" << data.to_string();
	}

	do_similarity_penalty(&leader_data);
	do_diversity_and_randomness_balancing(&leader_data);

	LOG_AI_FLIX << "Scores after extra treatments:\n";
	BOOST_FOREACH(const data& data, leader_data) {
		LOG_AI_FLIX << "\n" << data.to_string();
	}

	/**
	 * Step 4: Do recruitment according to scores and the leaders ratio_scores.
	 * Note that the scores don't indicate the preferred mix to recruit but rather
	 * the preferred mix of all units. So already existing units are considered.
	 */

	action_result_ptr action_result;
	config* job = NULL;
	do {
		recruit_situation_change_observer_.reset_gamestate_changed();
		update_state();
		if (state_ == SAVE_GOLD && ACTIVATE_SAVE_GOLD_STRATEGIES) {
			break;
		}
		job = get_most_important_job();
		if (!job) {
			LOG_AI_FLIX << "All recruitment jobs (recruitment_instructions) done.\n";
			break;
		}
		LOG_AI_FLIX << "Executing this job:\n" << *job << "\n";

		data* best_leader_data = get_best_leader_from_ratio_scores(leader_data, job);
		if (!best_leader_data) {
			LOG_AI_FLIX << "Leader with job (recruitment_instruction) is not on keep.\n";
			if (remove_job_if_no_blocker(job)) {
				continue;
			} else {
				break;
			}
		}
		const std::string best_recruit = get_best_recruit_from_scores(*best_leader_data, job);
		if (best_recruit.empty()) {
			LOG_AI_FLIX << "Cannot fullfil recruitment-instruction.\n";
			if (remove_job_if_no_blocker(job)) {
				continue;
			} else {
				break;
			}
		}

		// TODO(flix): find the best hex for recruiting best_recruit.
		// see http://forums.wesnoth.org/viewtopic.php?f=8&t=36571&p=526035#p525946
		// "It also means there is a tendency to recruit from the outside in
		// rather than the default inside out."

		LOG_AI_FLIX << "Best recruit is: " << best_recruit << "\n";
		const std::string* recall_id = get_appropriate_recall(best_recruit, *best_leader_data);
		if (recall_id) {
			LOG_AI_FLIX << "Found appropriate recall with id: " << *recall_id << "\n";
			action_result = execute_recall(*recall_id, *best_leader_data);
		} else {
			action_result = execute_recruit(best_recruit, *best_leader_data);
		}

		if (action_result->is_ok()) {
			++own_units_count_[best_recruit];
			++total_own_units_;

			// Update the current job.
			if (!job->operator[]("total").to_bool()) {
				job->operator[]("number") = job->operator[]("number").to_int(99999) - 1;
			}

			// Check if something changed in the recruitment list (WML can do that).
			// If yes, just return/break. evaluate() and execute() will be called again.
			if (recruit_situation_change_observer_.recruit_list_changed()) {
				break;
			}
			// Check if the gamestate changed more than once.
			// (Recruitment will trigger one gamestate change, WML could trigger more changes.)
			// If yes, just return/break. evaluate() and execute() will be called again.
			if (recruit_situation_change_observer_.gamestate_changed() > 1) {
				break;
			}

		} else {
			LOG_AI_FLIX << "Recruit result not ok.\n";
			// We'll end up here when
			// 1. We haven't enough gold,
			// 2. There aren't any free hexes around leaders,
			// 3. This leader can not recruit this type (this can happen after a recall)

			// TODO(flix): here something is needed to decide if we may want to recruit a
			// cheaper unit, when recruitment failed because of unit costs.
		}
	} while(action_result->is_ok());

	// Recruiting is done now.
	// Update state_ for next execution().

	if (state_ == LEADER_IN_DANGER) {
		state_ = NORMAL;
	}

	int status = (action_result) ? action_result->get_status() : -1;
	bool no_gold = (status == recruit_result::E_NO_GOLD || status == recall_result::E_NO_GOLD);
	if (state_ == SPEND_ALL_GOLD && no_gold) {
		state_ = SAVE_GOLD;
	}
	if (job && no_gold) {
		remove_job_if_no_blocker(job);
	}
}

/**
 * A helper function for execute().
 */
action_result_ptr recruitment::execute_recall(const std::string& id, data& leader_data) {
	recall_result_ptr recall_result;
	recall_result = check_recall_action(id, map_location::null_location,
			leader_data.leader->get_location());
	if (recall_result->is_ok()) {
		recall_result->execute();
		++leader_data.recruit_count;
	}
	return recall_result;
}

/**
 * A helper function for execute().
 */
action_result_ptr recruitment::execute_recruit(const std::string& type, data& leader_data) {
	recruit_result_ptr recruit_result;
	recruit_result = check_recruit_action(type, map_location::null_location,
			leader_data.leader->get_location());

	if (recruit_result->is_ok()) {
		recruit_result->execute();
		LOG_AI_FLIX << "Recruited " << type << "\n";
		++leader_data.recruit_count;
	}
	return recruit_result;
}

/**
 * A helper function for execute().
 * Checks if this unit type can be recalled.
 * If yes, we calculate a estimated value in gold of the recall unit.
 * If this value is less then the recall cost, we dismiss the unit.
 * The unit with the highest value will be returned.
 */
const std::string* recruitment::get_appropriate_recall(const std::string& type,
		const data& leader_data) const {
	const std::string* best_recall_id = NULL;
	double best_recall_value = -1;
	BOOST_FOREACH(const unit& recall_unit, current_team().recall_list()) {
		if (type != recall_unit.type_id()) {
			continue;
		}
		// Check if this leader is allowed to recall this unit.
		vconfig filter = vconfig(leader_data.leader->recall_filter());
		if (!recall_unit.matches_filter(filter, map_location::null_location)) {
			LOG_AI_FLIX << "Refused recall because filter: " << recall_unit.id() << "\n";
			continue;
		}
		double average_cost_of_advanced_unit = 0;
		int counter = 0;
		BOOST_FOREACH(const std::string& advancement, recall_unit.advances_to()) {
			const unit_type* advancement_type = unit_types.find(advancement);
			if (!advancement_type) {
				continue;
			}
			average_cost_of_advanced_unit += advancement_type->cost();
			++counter;
		}
		if (counter > 0) {
			average_cost_of_advanced_unit /= counter;
		} else {
			// Unit don't have advancements. Use cost of unit itself.
			average_cost_of_advanced_unit = recall_unit.cost();
		}
		double xp_quantity = static_cast<double>(recall_unit.experience()) /
				recall_unit.max_experience();
		double recall_value = recall_unit.cost() + xp_quantity * average_cost_of_advanced_unit;
		if (recall_value < current_team().recall_cost()) {
			continue;  // Unit is not worth to get recalled.
		}
		if (recall_value > best_recall_value) {
			best_recall_id = &recall_unit.id();
			best_recall_value = recall_value;
		}
	}
	return best_recall_id;
}

/**
 * A helper function for execute().
 * Decides according to the leaders ratio scores which leader should recruit.
 */
data* recruitment::get_best_leader_from_ratio_scores(std::vector<data>& leader_data,
		const config* job) const {
	assert(job);
	// Find things for normalization.
	int total_recruit_count = 0;
	double ratio_score_sum = 0.0;
	BOOST_FOREACH(const data& data, leader_data) {
		ratio_score_sum += data.ratio_score;
		total_recruit_count += data.recruit_count;
	}
	assert(ratio_score_sum > 0.0);

	// Shuffle leader_data to break ties randomly.
	std::random_shuffle(leader_data.begin(), leader_data.end());

	// Find which leader should recruit according to ratio_scores.
	data* best_leader_data = NULL;
	double biggest_difference = -99999.;
	BOOST_FOREACH(data& data, leader_data) {
		if (!leader_matches_job(data, job)) {
			continue;
		}
		double desired_ammount = data.ratio_score / ratio_score_sum * (total_recruit_count + 1);
		double current_ammount = data.recruit_count;
		double difference = desired_ammount - current_ammount;
		if (difference > biggest_difference) {
			biggest_difference = difference;
			best_leader_data = &data;
		}
	}
	return best_leader_data;
}

/**
 * A helper function for execute().
 * Counts own units and then decides what unit should be recruited so that the
 * unit distribution approaches the given scores.
 */
const std::string recruitment::get_best_recruit_from_scores(const data& leader_data,
		const config* job) {
	assert(job);
	std::string pattern_type = get_random_pattern_type_if_exists(leader_data, job);
	std::string best_recruit = "";
	double biggest_difference = -99999.;
	BOOST_FOREACH(const score_map::value_type& i, leader_data.get_normalized_scores()) {
		const std::string& unit = i.first;
		const double score = i.second;

		if (!limit_ok(unit)) {
			continue;
		}
		if (!pattern_type.empty()) {
			if (!recruit_matches_type(unit, pattern_type)) {
				continue;
			}
		} else {
			if (!recruit_matches_job(unit, job)) {
				continue;
			}
		}

		double desired_ammount = score * (total_own_units_ + 1);
		double current_ammount = own_units_count_[unit];
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
 * Helper function.
 * Returns true if there is a enemy within the radius.
 */
bool recruitment::is_enemy_in_radius(const map_location& loc, int radius) const {
	const unit_map& units = *resources::units;
	std::vector<map_location> surrounding;
	get_tiles_in_radius(loc, radius, surrounding);
	if (surrounding.empty()) {
		return false;
	}
	BOOST_FOREACH(const map_location& loc, surrounding) {
		const unit_map::const_iterator& enemy_it = units.find(loc);
		if(enemy_it == units.end()) {
			continue;
		}
		if (!current_team().is_enemy(enemy_it->side())) {
			continue;
		}
		return true;
	}
	return false;
}

/*
 * Helper Function.
 * Counts own units on the map and saves the result
 * in member own_units_count_
 */
void recruitment::update_own_units_count() {
	own_units_count_.clear();
	total_own_units_ = 0;
	const unit_map& units = *resources::units;
	BOOST_FOREACH(const unit& unit, units) {
		if (unit.side() != get_side() || unit.can_recruit()) {
			continue;
		}
		++own_units_count_[unit.type_id()];
		++total_own_units_;
	}
}

/**
 * For Configuration / Aspect "recruitment-instructions"
 * We call a [recruit] tag a "job".
 */
config* recruitment::get_most_important_job() {
	config* most_important_job = NULL;
	int most_important_importance = -1;
	int biggest_number = -1;
	BOOST_FOREACH(config& job, recruitment_instructions_.child_range("recruit")) {
		if (job.empty()) {
			continue;
		}
		int importance = job["importance"].to_int(1);
		int number = job["number"].to_int(99999);
		bool total = job["total"].to_bool(false);
		if (total) {
			// If the total flag is set we have to subtract
			// all existing units which matches the type.
			update_own_units_count();
			BOOST_FOREACH(const count_map::value_type& entry, own_units_count_) {
				const std::string& unit_type = entry.first;
				const int count = entry.second;
				if (recruit_matches_job(unit_type, &job)) {
					number = number - count;
				}
			}
		}
		if (number <= 0) {
			continue;
		}
		if (importance > most_important_importance ||
				(importance == most_important_importance && biggest_number > number)) {
			most_important_job = &job;
			most_important_importance = importance;
			biggest_number = number;
		}
	}
	return most_important_job;
}

/**
 * For Configuration / Aspect "recruitment-instructions"
 * If the flag pattern is set, this method returns a random element of the
 * type-attribute.
 */
const std::string recruitment::get_random_pattern_type_if_exists(const data& leader_data,
		const config* job) const {
	std::string choosen_type;
	if (job->operator[]("pattern").to_bool(false)) {
		std::vector<std::string> job_types = utils::split(job->operator[]("type"));

		// Before we choose a random pattern type, we make sure that at least one recruit
		// matches the types and doesn't exceed the [limit].
		// We do this by erasing elements of job_types.
		std::vector<std::string>::iterator job_types_it = job_types.begin();

		// Iteration through all elements.
		while (job_types_it != job_types.end()) {
			bool type_ok = false;
			BOOST_FOREACH(const std::string& recruit, leader_data.recruits) {
				if (recruit_matches_type(recruit, *job_types_it) && limit_ok(recruit)) {
					type_ok = true;
					break;
				}
			}
			if (type_ok) {
				++job_types_it;
			} else {
				// Erase Element. erase() will return iterator of next element.
				job_types_it = job_types.erase(job_types_it);
			}
		}

		if (!job_types.empty()) {
			// Choose a random job_type.
			choosen_type = job_types[rand() % job_types.size()];
		}
	}
	return choosen_type;
}

/**
 * For Configuration / Aspect "recruitment-instructions"
 * Checks if a given leader is specified in the "leader_id" attribute.
 */
bool recruitment::leader_matches_job(const data& leader_data, const config* job) const {
	assert(job);
	// First we make sure that this leader can recruit
	// at least one unit-type specified in the job.
	bool is_ok = false;
	BOOST_FOREACH(const std::string& recruit, leader_data.recruits) {
		if (recruit_matches_job(recruit, job) && limit_ok(recruit)) {
			is_ok = true;
			break;
		}
	}
	if (!is_ok) {
		return false;
	}

	std::vector<std::string> ids = utils::split(job->operator[]("leader_id"));
	if (ids.empty()) {
		// If no leader is specified, all leaders are okay.
		return true;
	}
	return (std::find(ids.begin(), ids.end(), leader_data.leader->id()) != ids.end());
}

/**
 * For Configuration / Aspect "recruitment-instructions"
 * Checks if a recruit-type can be recruited according to the [limit]-tag.
 */
bool recruitment::limit_ok(const std::string& recruit) const {
	// We don't use the member recruitment_instruction_ but instead
	// retrieve the aspect again. So the [limit]s can be altered during a turn.
	const config aspect = get_recruitment_instructions();

	BOOST_FOREACH(const config& limit, aspect.child_range("limit")) {
		if (recruit_matches_type(recruit, limit["type"])) {
			// Count all own existing units which matches the type.
			int count = 0;
			BOOST_FOREACH(const count_map::value_type& entry, own_units_count_) {
				const std::string& unit = entry.first;
				int number = entry.second;
				if (recruit_matches_type(unit, limit["type"])) {
					count += number;
				}
			}
			// Check if we reached the limit.
			if (count >= limit["max"]) {
				return false;
			}
		}
	}
	return true;
}

/**
 * For Configuration / Aspect "recruitment-instructions"
 * Checks if a given recruit-type is specified in the "type" attribute.
 */
bool recruitment::recruit_matches_job(const std::string& recruit, const config* job) const {
	assert(job);
	std::vector<std::string> job_types = utils::split(job->operator[]("type"));
	if (job_types.empty()) {
		// If no type is specified, all recruits are okay.
		return true;
	}
	BOOST_FOREACH(const std::string& job_type, job_types) {
		if (recruit_matches_type(recruit, job_type)) {
			return true;
		}
	}
	return false;
}
/**
 * For Configuration / Aspect "recruitment-instructions"
 * Checks if a given recruit-type matches one atomic "type" attribute.
 */
bool recruitment::recruit_matches_type(const std::string& recruit, const std::string& type) const {
	const unit_type* recruit_type = unit_types.find(recruit);
	if (!recruit_type) {
		return false;
	}
	// Consider type-name.
	if (recruit_type->id() == type) {
		return true;
	}
	// Consider usage.
	if (recruit_type->usage() == type) {
		return true;
	}
	// Consider level.
	std::stringstream s;
	s << recruit_type->level();
	if (s.str() == type) {
		return true;
	}
	return false;
}
/**
 * For Configuration / Aspect "recruitment-instructions"
 */
bool recruitment::remove_job_if_no_blocker(config* job) {
	assert(job);
	if (!job->operator[]("blocker").to_bool(true)) {
		LOG_AI_FLIX << "Canceling job.\n";
		job->clear();
		return true;
	} else {
		LOG_AI_FLIX << "Aborting recruitment.\n";
		return false;
	}
}

/**
 * For Map Analysis.
 * Creates a std::set of hexes where a fight will occur with high probability.
 */
void recruitment::update_important_hexes() {
	important_hexes_.clear();
	important_terrain_.clear();
	own_units_in_combat_counter_ = 0;

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
		if (is_enemy_in_radius(unit.get_location(), 1)) {
			// We found a enemy next to us. Mark our unit and all adjacent
			// hexes as important.
			std::vector<map_location> surrounding;
			get_tiles_in_radius(unit.get_location(), 1, surrounding);
			important_hexes_.insert(unit.get_location());
			std::copy(surrounding.begin(), surrounding.end(),
					std::inserter(important_hexes_, important_hexes_.begin()));
			++own_units_in_combat_counter_;
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
	unsigned int unit_count = 0;
	BOOST_FOREACH(const unit& unit, units) {
		if (unit.side() != side || unit.can_recruit()) {
			continue;
		}
		++unit_count;
		cost_map.add_unit(unit);
	}

	// If this side has not so many units yet, add unit_types with the leaders position as origin.
	if (unit_count < UNIT_THRESHOLD) {
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
 * Calculates a average lawful bonus, so Combat Analysis will work
 * better in caves and custom time of day cycles.
 */
void recruitment::update_average_lawful_bonus() {
	int sum = 0;
	int counter = 0;
	BOOST_FOREACH(const time_of_day& time, resources::tod_manager->times()) {
		sum += time.lawful_bonus;
		++counter;
	}
	if (counter > 0) {
		average_lawful_bonus_ = round_double(static_cast<double>(sum) / counter);
	}
}

/**
 * Combat Analysis.
 * Main function.
 * Compares all enemy units with all of our possible recruits and fills
 * the scores.
 */
void recruitment::do_combat_analysis(std::vector<data>* leader_data) {
	const unit_map& units = *resources::units;

	// Collect all enemy units (and their hp) we want to take into account in enemy_units.
	typedef std::vector<std::pair<std::string, int> > unit_hp_vector;
	unit_hp_vector enemy_units;
	BOOST_FOREACH(const unit& unit, units) {
		if (!current_team().is_enemy(unit.side())) {
			continue;
		}
		enemy_units.push_back(std::make_pair(unit.type_id(), unit.hitpoints()));
	}
	if (enemy_units.size() < UNIT_THRESHOLD) {
		// Use also enemies recruitment lists and insert units into enemy_units.
		BOOST_FOREACH(const team& team, *resources::teams) {
			if (!current_team().is_enemy(team.side())) {
				continue;
			}
			std::set<std::string> possible_recruits;
			// Add team recruits.
			possible_recruits.insert(team.recruits().begin(), team.recruits().end());
			// Add extra recruits.
			const std::vector<unit_map::const_iterator> leaders = units.find_leaders(team.side());
			BOOST_FOREACH(unit_map::const_iterator leader, leaders) {
				possible_recruits.insert(leader->recruits().begin(), leader->recruits().end());
			}
			// Insert set in enemy_units.
			BOOST_FOREACH(const std::string& possible_recruit, possible_recruits) {
				const unit_type* recruit_type = unit_types.find(possible_recruit);
				if (recruit_type) {
					int hp = recruit_type->hitpoints();
					enemy_units.push_back(std::make_pair(possible_recruit, hp));
				}
			}
		}
	}

	BOOST_FOREACH(data& leader, *leader_data) {
		if (leader.recruits.empty()) {
			continue;
		}
		typedef std::map<std::string, double> simple_score_map;
		simple_score_map temp_scores;

		BOOST_FOREACH(const unit_hp_vector::value_type& entry, enemy_units) {
			const std::string& enemy_unit = entry.first;
			int enemy_unit_hp = entry.second;

			std::string best_response;
			double best_response_score = -99999.;
			BOOST_FOREACH(const std::string& recruit, leader.recruits) {
				double score = compare_unit_types(recruit, enemy_unit);
				score *= enemy_unit_hp;
				score = pow(score, COMBAT_SCORE_POWER);
				temp_scores[recruit] += (COMBAT_DIRECT_RESPONSE) ? 0 : score;
				if (score > best_response_score) {
					best_response_score = score;
					best_response = recruit;
				}
			}
			if (COMBAT_DIRECT_RESPONSE) {
				temp_scores[best_response] += enemy_unit_hp;
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
			const attack_type* att_weapon, const attack_type* def_weapon,
			int average_lawful_bonus) :
			attacker_type(attacker),
			defender_type(defender),
			attacker_stats(attacker, att_weapon, true, defender, def_weapon,
					round_double(defender_defense), average_lawful_bonus),
			defender_stats(defender, def_weapon, false, attacker, att_weapon,
					round_double(attacker_defense), average_lawful_bonus),
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
					&att_weapon, &def_weapon, average_lawful_bonus_));
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
					&att_weapon, NULL, average_lawful_bonus_));
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
 * For Gold Saving Strategies.
 * Guess the income over the next turns.
 * This doesn't need to be exact. In the end we are just interested if this value is
 * positive or negative.
 */
double recruitment::get_estimated_income(int turns) const {
	const team& team = (*resources::teams)[get_side() - 1];
	const size_t own_villages = team.villages().size();
	const double village_gain = get_estimated_village_gain();
	const double unit_gain = get_estimated_unit_gain();

	double total_income = 0;
	for (int i = 1; i <= turns; ++i) {
		double income = (own_villages + village_gain * i) * game_config::village_income;
		double upkeep = side_upkeep(get_side()) + unit_gain * i -
				(own_villages + village_gain * i) * game_config::village_support;
		double resulting_income = game_config::base_income + income - std::max(0., upkeep);
		total_income += resulting_income;
	}
	return total_income;
}

/**
 * For Gold Saving Strategies.
 * Guess how many units we will gain / loose over the next turns per turn.
 */
double recruitment::get_estimated_unit_gain() const {
	return - own_units_in_combat_counter_ / 3.;
}

/**
 * For Gold Saving Strategies.
 * Guess how many villages we will gain over the next turns per turn.
 */
double recruitment::get_estimated_village_gain() const {
	const gamemap& map = *resources::game_map;
	int neutral_villages = 0;
	BOOST_FOREACH(const map_location& village, map.villages()) {
		if (village_owner(village) == -1) {
			++neutral_villages;
		}
	}
	return (neutral_villages / resources::teams->size()) / 4.;
}

/**
 * For Gold Saving Strategies.
 * Returns our_total_unit_costs / enemy_total_unit_costs.
 */
double recruitment::get_unit_ratio() const {
	const unit_map& units = *resources::units;
		double own_total_value;
		double enemy_total_value;
		BOOST_FOREACH(const unit& unit, units) {
			double value = unit.cost() * unit.hitpoints() / unit.max_hitpoints();
			if (current_team().is_enemy(unit.side())) {
				enemy_total_value += value;
			} else {
				own_total_value += value;
			}
		}
	return own_total_value / enemy_total_value;
}

/**
 * Gold Saving Strategies. Main method.
 */
void recruitment::update_state() {
	if (state_ == LEADER_IN_DANGER || state_ == SPEND_ALL_GOLD) {
		return;
	}
	int threshold = (SPEND_ALL_GOLD_GOLD_THRESHOLD < 0) ?
			current_team().start_gold() + 1: SPEND_ALL_GOLD_GOLD_THRESHOLD;
	if (current_team().gold() >= threshold) {
		state_ = SPEND_ALL_GOLD;
		LOG_AI_FLIX << "Changed state_ to SPEND_ALL_GOLD. \n";
		return;
	}
	double ratio = get_unit_ratio();
	double income_estimation = get_estimated_income(SAVE_GOLD_FORECAST_TURNS);
	LOG_AI_FLIX << "Ratio is " << ratio << "\n";
	LOG_AI_FLIX << "Estimated income is " << income_estimation << "\n";
	if (state_ == NORMAL && ratio > SAVE_GOLD_BEGIN_THRESHOLD && income_estimation > 0) {
		state_ = SAVE_GOLD;
		LOG_AI_FLIX << "Changed state to SAVE_GOLD.\n";

		// Create a debug object. // REMOVE ME
		debug.estimated_income = get_estimated_income(SAVE_GOLD_FORECAST_TURNS);
		debug.estimated_unit_gain = get_estimated_unit_gain();
		debug.estimated_village_gain = get_estimated_village_gain();
		debug.turn_start =  resources::tod_manager->turn();
		const unit_map& units = *resources::units;
		int own_units = 0;
		BOOST_FOREACH(const unit& unit, units) {
			if (get_side() == unit.side()) {
				++own_units;
			}
		}
		debug.units = own_units;
		debug.villages = (*resources::teams)[get_side() - 1].villages().size();
		debug.gold = current_team().gold();
	} else if (state_ == SAVE_GOLD && ratio < SAVE_GOLD_END_THRESHOLD) {
		state_ = NORMAL;
		LOG_AI_FLIX << "Changed state to NORMAL.\n";

		// Use the debug object to create a output. // REMOVE ME
		LOG_AI_FLIX << "-------EVALUATE ESTIMATIONS-----------\n";
		LOG_AI_FLIX << "Turns: " << resources::tod_manager->turn() - debug.turn_start << "\n";
		LOG_AI_FLIX << "EVG: " << debug.estimated_village_gain << ", RVG: " << (*resources::teams)[get_side() - 1].villages().size() - debug.villages << "\n";
		const unit_map& units = *resources::units;
		int own_units = 0;
		BOOST_FOREACH(const unit& unit, units) {
			if (get_side() == unit.side()) {
				++own_units;
			}
		}
		LOG_AI_FLIX << "EUG: " << debug.estimated_unit_gain << ", RUG: " << own_units - debug.units << "\n";
		LOG_AI_FLIX << "EI: " << debug.estimated_income << ", RI: " << current_team().gold() - debug.gold << "\n";
		LOG_AI_FLIX << "----------------END---------------------\n";
	}
}

/**
 * Will add a offset (DIVERSITY_WEIGHT * 50) to all scores so
 * overall recruitment will be more diverse.
 */
void recruitment::do_diversity_and_randomness_balancing(std::vector<data>* leader_data) const {
	if (!leader_data) {
		return;
	}
	BOOST_FOREACH(data& data, *leader_data) {
		BOOST_FOREACH(score_map::value_type& entry, data.scores) {
			double& score = entry.second;
			score += DIVERSITY_WEIGHT * 50;
			score += (static_cast<double>(rand()) / RAND_MAX) * RANDOMNESS_MAXIMUM;
		}
	}
}

/**
 * Will give a penalty to similar units. Similar units are units in one advancement tree.
 * Example (Archer can advance to Ranger):
 *                 before    after
 * Elvish Fighter:   50        50
 * Elvish Archer:    50        25
 * Elvish Ranger:    50        25
 */
void recruitment::do_similarity_penalty(std::vector<data>* leader_data) const {
	if (!leader_data) {
		return;
	}
	BOOST_FOREACH(data& data, *leader_data) {
		// First we count how many similarities each recruit have to other ones (in a map).
		// Some examples:
		// If unit A and unit B have nothing to do with each other, they have similarity = 0.
		// If A advances to B both have similarity = 1.
		// If A advances to B and B to C, A, B and C have similarity = 2.
		// If A advances to B or C, A have similarity = 2. B and C have similarity = 1.
		typedef std::map<std::string, int> similarity_map;
		similarity_map similarities;
		BOOST_FOREACH(const score_map::value_type& entry, data.scores) {
			const std::string& recruit = entry.first;
			const unit_type* recruit_type = unit_types.find(recruit);
			if (!recruit_type) {
				continue;
			}
			BOOST_FOREACH(const std::string& advanced_type, recruit_type->advancement_tree()){
				if (data.scores.count(advanced_type) != 0) {
					++similarities[recruit];
					++similarities[advanced_type];
				}
			}
		}
		// Now we divide each score by similarity + 1.
		BOOST_FOREACH(score_map::value_type& entry, data.scores) {
			const std::string& recruit = entry.first;
			double& score = entry.second;
			score /= (similarities[recruit] + 1);
		}
	}
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
