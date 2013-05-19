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
 * Uses Game Theory to find the best mix to recruit
 * when only the enemies recruitment-list is available.
 *
 * See http://wiki.wesnoth.org/User:Flixx/Game_Theory_for_Recruiting
 * for a full explanation
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
#include <math.h>
#include <iomanip>

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
		cfg_poisson_turns_ = (cfg.has_attribute("poisson_turns")) ? cfg["poisson_turns"] : 1;
		LOG_AI_FLIX << "poisson_turns = " << cfg_poisson_turns_ << "\n";
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

void recruitment::print_table(effectivness_table& table) const {

	//std::setw() does big magic here to make everything look good.

	std::stringstream out;
	out << std::setprecision(4);

	//first row
	out << "          my/enemy";
	table_row first_row = (table.begin())->second;
	BOOST_FOREACH(table_cell& i, first_row){
		std::string col_name = i.first;
		out << "|" << std::setw(15) << col_name << std::setw(3);
	}

	//other rows
	BOOST_FOREACH(table_row_pair& i, table){
		std::string row_name = i.first;
		table_row row = i.second;
		out << "\n" << std::setw(18) << row_name;
		BOOST_FOREACH(table_cell& cell, row){
			double val = cell.second;
			out << "|" << std::setw(10) << val << std::setw(8);
		}
	}

	DBG_AI_FLIX<< "\n" << out.str() << "\n";
}

void recruitment::execute() {
		LOG_AI_FLIX << "Flix recruitment begin! \n";

		const std::vector<team> &teams = *resources::teams;

		const std::set<std::string> own_recruits = current_team().recruits();
		std::set<std::string> enemy_recruits;

		//for now we only use the recruit list of one enemy (the first found)
		//later one can think about including multiple enemies
		BOOST_FOREACH(const team& team, teams){
			if(current_team().is_enemy(team.side())){
				enemy_recruits = team.recruits();
			}
		}

		effectivness_table table;

		std::set<std::string>::const_iterator own_unit_s;
		std::set<std::string>::const_iterator enemy_unit_s;

		BOOST_FOREACH(const std::string& own_unit_s, own_recruits){
			BOOST_FOREACH(const std::string& enemy_unit_s, enemy_recruits){
				const unit_type *own_unit = unit_types.find(own_unit_s);
				const unit_type *enemy_unit = unit_types.find(enemy_unit_s);

				const double own_effectiveness_vs_enemy = average_resistance_against(*enemy_unit,*own_unit);
				const double enemy_effectiveness_vs_own = average_resistance_against(*own_unit, *enemy_unit);

				table[own_unit_s][enemy_unit_s] = own_effectiveness_vs_enemy - enemy_effectiveness_vs_own;
				DBG_AI_FLIX << own_unit_s << " : " << enemy_unit_s << " << " << own_effectiveness_vs_enemy << " - " << enemy_effectiveness_vs_own << " = " << table[own_unit_s][enemy_unit_s] << "\n";
			}
		}

		print_table(table);

		std::map<std::string, double> strategy = findEquilibrium(table);

		recruit_result_ptr recruit_result;
		do{
			//the following code will choose a random recruit
			//with probabilities according to strategy.
			int random = rand() % 10000;
			std::string recruit;

			for(std::map<std::string, double>::const_iterator i = strategy.begin(); i != strategy.end(); ++i){
				random -= (i->second * 10000);
				if(random <= 0){
					recruit = i->first;
					break;
				}
			}

			recruit_result = check_recruit_action(recruit);
			if(recruit_result->is_ok()){
				recruit_result->execute();
				LOG_AI_FLIX << "Recruited " << recruit << std::endl;
			}
		} while(recruit_result->is_ok());
	}

	recruitment::~recruitment()
	{
	}

	/**
	 * This function is basically a modified version of the same
	 * function in ca.*pp
	 *
	 * It used to find a weighted average of the damage
	 * from all attacks of unit b to unit a.
	 *
	 * Now it considers only the attack the attacker b will
	 * most likely do because the difference between attacker
	 * damage and defender damage is the greatest.
	 *
	 * Also it will weight the damage by the unit cost.
	 *
	 * The simplified output is:
	 * attack_damage_to_a / hp_of_a / cost_of_b - defense_demage_to_b / hp_of_b / cost_of_a;
	 *
	 * whereas the damages corresponds to the attacks the attacker will most likely choose.
	 *
	 * Note that this function needs a refactoring
	 * (the variable names are inconsistent and sometimes confunsing)
	 *
	 *a: defender
	 *b: attacker
	 */
	int recruitment::average_resistance_against(const unit_type& a, const unit_type& b) const
	{
		int weighting_sum_a = 0, defense_a = 0, weighting_sum_b = 0, defense_b = 0;
		const std::map<t_translation::t_terrain, size_t>& terrain =
			resources::game_map->get_weighted_terrain_frequencies();

		for (std::map<t_translation::t_terrain, size_t>::const_iterator j = terrain.begin(),
		     j_end = terrain.end(); j != j_end; ++j)
		{
			// Use only reachable tiles when computing the average defense.
		  if (a.movement_type().movement_cost(j->first) < movetype::UNREACHABLE) {
				defense_a += a.movement_type().defense_modifier(j->first) * j->second;
				weighting_sum_a += j->second;
				defense_b += b.movement_type().defense_modifier(j->first) * j->second;
				weighting_sum_b += j->second;
			}
		}

		if (weighting_sum_a == 0) {
			// This unit can't move on this map, so just get the average weighted
			// of all available terrains. This still is a kind of silly
			// since the opponent probably can't recruit this unit and it's a static unit.
			for (std::map<t_translation::t_terrain, size_t>::const_iterator jj = terrain.begin(),
					jj_end = terrain.end(); jj != jj_end; ++jj)
			{
				defense_a += a.movement_type().defense_modifier(jj->first) * jj->second;
				weighting_sum_a += jj->second;
			}
		}
		if (weighting_sum_b == 0) {
			// This unit can't move on this map, so just get the average weighted
			// of all available terrains. This still is a kind of silly
			// since the opponent probably can't recruit this unit and it's a static unit.
			for (std::map<t_translation::t_terrain, size_t>::const_iterator jj = terrain.begin(),
					jj_end = terrain.end(); jj != jj_end; ++jj)
			{
				defense_b += b.movement_type().defense_modifier(jj->first) * jj->second;
				weighting_sum_b += jj->second;
			}
		}

		if(weighting_sum_a != 0) {
			defense_a /= weighting_sum_a;
		} else {
			ERR_AI_FLIX << "The weighting sum is 0 and is ignored.\n";
		}
		if(weighting_sum_b != 0) {
			defense_b /= weighting_sum_b;
		} else {
			ERR_AI_FLIX << "The weighting sum is 0 and is ignored.\n";
		}

		int best_attack_damage = -99999;
		const attack_type * best_attack = NULL;

		// calculation of the average damage taken
		bool steadfast_a = a.has_ability_by_id("steadfast");
		bool poisonable_a = !a.musthave_status("unpoisonable");
		bool steadfast_b = b.has_ability_by_id("steadfast");
		bool poisonable_b = !b.musthave_status("unpoisonable");
		const std::vector<attack_type>& attacks_b = b.attacks();
		for (std::vector<attack_type>::const_iterator i = attacks_b.begin(), i_end = attacks_b.end(); i != i_end; ++i)
		{
			int resistance_a = a.movement_type().resistance_against(*i);
			// Apply steadfast resistance modifier.
			if (steadfast_a && resistance_a < 100)
				resistance_a = std::max<int>(resistance_a * 2 - 100, 50);
			// Do not look for filters or values, simply assume 70% if CTH is customized.
			int cth_a = i->get_special_bool("chance_to_hit", true) ? 70 : defense_a;
			int plain_damage_to_a = i->damage() * i->num_attacks();
			// if cth == 0 the division will do 0/0 so don't execute this part
			if (poisonable_a && cth_a != 0 && i->get_special_bool("poison", true)) {
				// Compute the probability of not poisoning the unit.
				double prob = 1;
				for (int m = 0; m < i->num_attacks(); ++m)
					prob = prob * (100 - cth_a) / 100;
				// Assume poison works one turn.
				plain_damage_to_a += game_config::poison_amount * prob;
			}
			LOG_AI_FLIX << "<<" << "a:" << a.type_name() << " b:" << b.type_name() << " << Attacker damage calc: cth * res * plain  " << cth_a << " * " << resistance_a << " * " << plain_damage_to_a << " = " << (cth_a * resistance_a * plain_damage_to_a) << "\n";
			int damage_to_a = cth_a * resistance_a * plain_damage_to_a;

			//now calculate the defense damage
			int max_defense_demage = 0;
			const attack_type * best_defense = NULL;
			const std::vector<attack_type>& attacks_a = a.attacks();
			for (std::vector<attack_type>::const_iterator j = attacks_a.begin(), j_end = attacks_a.end(); j != j_end; ++j)
			{
				//check if defense weapon is of the same type as attack weapon
				if(i->range() != j->range()){
					continue;
				}
				int resistance_b = b.movement_type().resistance_against(*j);
				// Apply steadfast resistance modifier.
				if (steadfast_b && resistance_b < 100){
					resistance_b = std::max<int>(resistance_b * 2 - 100, 50);
				}
				// Do not look for filters or values, simply assume 70% if CTH is customized.
				int cth_b = j->get_special_bool("chance_to_hit", true) ? 70 : defense_b;

				int plain_damage_to_b = j->damage() * j->num_attacks();
				// if cth == 0 the division will do 0/0 so don't execute this part
				if (poisonable_b && cth_b != 0 && j->get_special_bool("poison", true)) {
					// Compute the probability of not poisoning the unit.
					double prob = 1;
					for (int n = 0; n < j->num_attacks(); ++n){
						prob = prob * (100 - cth_b) / 100;
					}
					// Assume poison works one turn.
					plain_damage_to_b += game_config::poison_amount * prob;
				}
				int damage_to_b = cth_b * resistance_b * plain_damage_to_b; // average damage * weight
				if(damage_to_b > max_defense_demage){
					max_defense_demage = damage_to_b;
					best_defense = &(*j);
				}
			}

			LOG_AI_FLIX << "a:" << a.type_name() << " b:" << b.type_name() << " << Considering " << i->name() << " against " << ((best_defense) ? best_defense->name() : "#") << " << " << damage_to_a << " / " << a.hitpoints() << " / " << b.cost() <<  " - " << max_defense_demage << " / " << b.hitpoints() << " / " << a.cost() << "\n";

			// normalize defense damage by HP
			max_defense_demage /= std::max<int>(1,std::min<int>(b.hitpoints(),1000)); // avoid values really out of range
			// normalize attack damage by HP
			damage_to_a /= std::max<int>(1,std::min<int>(a.hitpoints(),1000)); // avoid values really out of range

			//normalize damage by costs
			damage_to_a /= b.cost();
			max_defense_demage /= a.cost();

			int attack_damage = damage_to_a - max_defense_demage;

			if(attack_damage > best_attack_damage){
				best_attack_damage = attack_damage;
				best_attack = &(*i);
			}
		}

		LOG_AI_FLIX << "a:" << a.type_name() << " b:" << b.type_name() << " << Best_attack: " << ((best_attack) ? best_attack->name() : "#") << ": " << best_attack_damage << "\n";

		return best_attack_damage;
	}

	/**
	 * This algorithm is based on
	 * http://www.rand.org/content/dam/rand/pubs/commercial_books/2007/RAND_CB113-1.pdf
	 * Site 219.
	 *
	 * The "steps" in this function are the same as in the book
	 *
	 * It will output a mixed strategy. (Unit-type => Percentage)
	 */

	const std::map<std::string, double> recruitment::findEquilibrium(effectivness_table& table) const{

		std::stringstream s;
		table_row first_row = (table.begin())->second;

		/**
		 * Preparing some datastructures for Step 5
		 */

		//Because we want to keep the keys of table immutable we cannot simply exchange them
		//like in Step 5 suggested. Instead we keep the "exchangeable names" in separate maps.
		std::map<std::string, std::string> names_above;
		std::map<std::string, std::string> names_below;
		std::map<std::string, std::string> names_left;
		std::map<std::string, std::string> names_right;

		BOOST_FOREACH(table_row_pair& i, table){
			std::string row_name = i.first;
			names_left[row_name] = row_name;
			names_right[row_name] = "";
		}

		BOOST_FOREACH(table_cell& i, first_row){
			std::string col_name = i.first;
			names_above[col_name] = col_name;
			names_below[col_name] = "";
		}

		/**
		 * Step1: Add offset so all values are not negative.
		 */

		double min = 0;


		BOOST_FOREACH(table_row_pair& i, table){
			table_row row = i.second;
			BOOST_FOREACH(table_cell& j, row){
				double val = j.second;
				if(val < min){
					min = val;
				}
			}
		}

		if(min < 0){
			BOOST_FOREACH(table_row_pair& i, table){
				table_row& row = i.second;
				BOOST_FOREACH(table_cell& j, row){
					double& val = j.second;
					val = val - min; //this will alter the table
				}
			}
		}

		LOG_AI_FLIX << "Table after Step 1: \n";
		print_table(table);

		/**
		 * Step 2: Adding cells, init D.
		 */

		BOOST_FOREACH(table_cell& i, first_row){
			std::string col_name = i.first;
			table["_"][col_name] = -1;
		}

		BOOST_FOREACH(table_row_pair& i, table){
			std::string row_name = i.first;
			table[row_name]["_"] = 1;
		}

		table["_"]["_"] = 0;

		double D = 1;

		LOG_AI_FLIX << "Table after Step 2: \n";
		print_table(table);
		LOG_AI_FLIX << "D = " << D << "\n";

		//this is the mainloop from step 3 to step 6.
		bool found;
		do{
			/**
			 * Step 3: Find pivot element
			 */

			//a map to save the smallest candidate pivot for each column
			//       columnname             rowname   pivot creteria
			std::map<std::string, std::pair<std::string, double> > column_mins;

			//iterate over columns
			BOOST_FOREACH(table_cell& i, first_row){
				std::string col_name = i.first;
				//The number at the foot of the column must be negative
				if(table["_"][col_name] >= 0){
					continue;
				}

				double column_min = INFINITY;
				std::string column_min_row_name;

				//iterate over rows
				BOOST_FOREACH(table_row_pair& j, table){ //for(effectivness_table::const_iterator j = table.begin(); j != table.end(); ++j){
					std::string row_name = j.first;

					//candidate pivot must be positive
					if(row_name == "_" || table[row_name][col_name] <= 0){
						continue;
					}
					//candidate = -(r * c) / p
					double candidate = -(table[row_name]["_"] * table["_"][col_name]) / table[row_name][col_name];

					if(candidate < column_min){
						column_min = candidate;
						column_min_row_name = row_name;
					}
				}//iterate over rows

				column_mins[col_name] = std::make_pair(column_min_row_name, column_min);

			}//iterate over columns

			//find Maximum in columnMins
			double max = - INFINITY;
			std::pair<std::string, std::string> pivot_index;
			for(std::map<std::string, std::pair<std::string, double> >::const_iterator i = column_mins.begin(); i != column_mins.end(); ++i){
				if((i->second).second > max){
					max = (i->second).second;
					pivot_index = std::make_pair((i->second).first, i->first);
				}
			}

			double pivot = table[pivot_index.first][pivot_index.second];

			//debug prints for Step 3
			s.str("");
			s << std::setprecision(3);
			s << "colum_mins: ";
			for(std::map<std::string, std::pair<std::string, double> >::const_iterator i = column_mins.begin(); i != column_mins.end(); ++i){
				s << i->first << ":<" << (i->second).first << ", " << (i->second).second << "> ";
			}
			s << "\n";
			LOG_AI_FLIX << "Step 3 finished.\n";
			LOG_AI_FLIX << s.str();
			LOG_AI_FLIX << "Pivot is " << pivot << " at (" << pivot_index.first << ", " << pivot_index.second << ")\n";

			/**
			 * Step 4: Updating Table
			 */

			//Step 4.1
			table[pivot_index.first][pivot_index.second] = D;

			//Step 4.2 : nothing to change

			//Step 4.3 and 4.4 are swapped because we need the
			//not negated columns in step 4.4

			//Step 4.4
			//iterating over each cell
			BOOST_FOREACH(table_row_pair& i, table){
				std::string row_name = i.first;
				table_row& row = i.second;

				BOOST_FOREACH(table_cell& j, row){
					std::string col_name = j.first;
					double& val = j.second;

					if(row_name == pivot_index.first || col_name == pivot_index.second){
						continue;
					}

					double N = val;
					double P = pivot;
					double R = table[pivot_index.first][col_name];
					double C = table[row_name][pivot_index.second];

					val = ((N * P) - (R * C)) / D; //this will alter the table
				}
			} //iterating over each cell

			//Step 4.3 : Negating Column
			BOOST_FOREACH(table_row_pair& i, table){
				std::string row_name = i.first;

				if(row_name == pivot_index.first){
					continue;
				}
				table[row_name][pivot_index.second] = - table[row_name][pivot_index.second];
			}

			//Step 4.5
			D = pivot;

			LOG_AI_FLIX << "Table after Step 4\n";
			print_table(table);
			LOG_AI_FLIX << "D = " << D << "\n";

			/**
			 * Step 5: Exchanging Names
			 */

			std::swap(names_left[pivot_index.first], names_below[pivot_index.second]);
			std::swap(names_above[pivot_index.second], names_right[pivot_index.first]);

			//debug prints Step 5
			s.str("");
			s << "names_above : ";
			for(std::map<std::string, std::string>::const_iterator i = names_above.begin(); i != names_above.end(); ++i){
				s << " | " << i->second;
			}
			LOG_AI_FLIX << s.str() << " |\n";

			s.str("");
			s << "names_below : ";
			for(std::map<std::string, std::string>::const_iterator i = names_below.begin(); i != names_below.end(); ++i){
				s << " | " << i->second;
			}
			LOG_AI_FLIX << s.str() << " |\n";

			s.str("");
			s << "names_left : ";
			for(std::map<std::string, std::string>::const_iterator i = names_left.begin(); i != names_left.end(); ++i){
				s << " | " << i->second;
			}
			LOG_AI_FLIX << s.str() << " |\n";

			s.str("");
			s << "names_right : ";
			for(std::map<std::string, std::string>::const_iterator i = names_right.begin(); i != names_right.end(); ++i){
				s << " | " << i->second;
			}
			LOG_AI_FLIX << s.str() << " |\n";

			/**
			 * Step 6: Check for exit-conditions
			 */

			found = true;

			BOOST_FOREACH(table_row_pair& i, table){
				std::string row_name = i.first;
				if(table[row_name]["_"] < 0){
					found = false;
				}
			}

			BOOST_FOREACH(table_cell& i, first_row){
				std::string col_name = i.first;
				if(table["_"][col_name] < 0){
					found = false;
				}
			}

		} while(!found);

		LOG_AI_FLIX << "Found Equilibrium!\n";

		//build Strategy
		std::map<std::string, double> strategy = std::map<std::string, double>();
		double sum = 0;

		//iterate over names_below
		for(std::map<std::string, std::string>::const_iterator i = names_below.begin(); i != names_below.end(); ++i){
			if(i->second != ""){
				strategy[i->second] = table["_"][i->first];
				sum += table["_"][i->first];
			}
		}

		s.str("");
		s << "Strategy is: ";

		for(std::map<std::string, double>::iterator i = strategy.begin(); i != strategy.end(); ++i){
			//normalize so sum of probabilities is 1
			i->second /= sum;

			s << i->first << ": " << (i->second * 100) << "%  ";
		}
		LOG_AI_FLIX << s.str() << "\n";

		return strategy;
	}

}

}
