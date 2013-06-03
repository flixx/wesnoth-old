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
 * Experimental Recruitment Engine by Flix
 *
 * @file
 */


#include "../../resources.hpp"
#include "../../map.hpp"
#include "../../unit_map.hpp"
#include "../../unit_types.hpp"
#include "../../log.hpp"
#include "../../team.hpp"

#include "../composite/rca.hpp"
#include "../actions.hpp"

#include "recruitment.hpp"

#include <boost/foreach.hpp>

static lg::log_domain log_ai_flix("ai/flix");
#define LOG_AI_FLIX LOG_STREAM(info, log_ai_flix)
#define DBG_AI_FLIX LOG_STREAM(debug, log_ai_flix)
#define ERR_AI_FLIX LOG_STREAM(err, log_ai_flix)

#ifdef _MSC_VER
#pragma warning(push)
//silence "inherits via dominance" warnings
#pragma warning(disable:4250)
#endif

namespace ai {

namespace flix_recruitment {

	recruitment::recruitment( rca_context &context , const config &cfg ):
		candidate_action(context, cfg){

	}
	double recruitment::evaluate(){
		std::vector<unit_map::unit_iterator> leaders = resources::units->find_leaders(get_side());

		BOOST_FOREACH(unit_map::unit_iterator &leader, leaders){
			if (leader == resources::units->end()) {
				return BAD_SCORE;
			}

			std::set<map_location> checked_hexes;
			const map_location &keep = leader->get_location();
			checked_hexes.insert(keep);

			if (resources::game_map->is_keep(leader->get_location()) && count_free_hexes_in_castle(leader->get_location(), checked_hexes) != 0) {
				return get_score();
			}
		}

		return BAD_SCORE;
	}

	void recruitment::execute() {
		LOG_AI_FLIX << "Flix recruitment begin! \n";

		const std::set<std::string> own_recruits = current_team().recruits();



		/**
		 * Step 1: Find important hexes and calculate other static things.
		 * Maybe cache it for later use.
		 * (TODO)
		 */

		/**
		 * Step 2: Filter own_recruits according to configurations
		 * (TODO)
		 */

		score_map scores;

		/**
		 * Step 3: Fill scores with values coming from combat analysis and other stuff.
		 */

		//For now just fill the scores with dummy entries:
		//The first two units get a score of 1000, all others 0

		int idx = 0;
		BOOST_FOREACH(std::string own_recruit, own_recruits){
			scores[own_recruit] = (idx < 2) ? 1000 : 0;
			idx++;
		}


		/**
		 * Step 4: Do recruitment according to scores.
		 * Note that the scores indicate the preferred mix to recruit but rather
		 * the preferred mix of all units. So already existing units are considered.
		 */

		//Count all own units which are already on the map
		std::map<std::string, int> own_units_count;
		double own_units_total = 0;

		const unit_map &units = *resources::units;

		BOOST_FOREACH(const unit& unit, units){
			if(unit.side() != current_team().side() || unit.can_recruit()){
				continue;
			}

			++own_units_count[unit.type_name()];
			++own_units_total;
		}

		//Calculate sum of scores
		double scores_total = 0;
		BOOST_FOREACH(score_map::value_type entry, scores){
			double score = entry.second;

			scores_total += score;
		}
		assert(scores_total > 0);

		recruit_result_ptr recruit_result;
		do{
			std::string best_recruit;
			double biggest_difference = -100;
			BOOST_FOREACH(score_map::value_type entry, scores){
				std::string unit = entry.first;
				double score = entry.second;

				double should_be = (score / scores_total) * 100; // %
				double is = (own_units_total == 0) ? 0 : (own_units_count[unit] / own_units_total) * 100; // %
				double difference = should_be - is;

				if(difference > biggest_difference){
					biggest_difference = difference;
					best_recruit = unit;
				}
			}

			recruit_result = check_recruit_action(best_recruit);
			if(recruit_result->is_ok()){
				recruit_result->execute();
				LOG_AI_FLIX << "Recruited " << best_recruit << "\n";
				++own_units_count[best_recruit];
				++own_units_total;
			}
		} while(recruit_result->is_ok());
	}

	recruitment::~recruitment()
	{
	}
}

}
