/*
 * recruitment.cpp
 *
 *  Created on: Apr 16, 2013
 *      Author: fehlx
 */

#include <boost/foreach.hpp>
#include <map>

#include "../../actions/attack.hpp"
#include "../../attack_prediction.hpp"
#include "../../resources.hpp"
#include "../../log.hpp"
#include "../../map.hpp"
#include "../../team.hpp"
#include "../../unit_display.hpp"
#include "../../unit_map.hpp"
#include "../../unit_types.hpp"
#include "../composite/rca.hpp"
#include "../default/ai.hpp"
#include <math.h>
#include <iomanip>
#include "recruitment2.hpp"

#include "../actions.hpp"
#include "../manager.hpp"
#include "../composite/engine.hpp"
#include "../composite/rca.hpp"
#include "../composite/stage.hpp"
#include "../../gamestatus.hpp"
#include "../../log.hpp"
#include "../../map.hpp"
#include "../../resources.hpp"
#include "../../team.hpp"

static lg::log_domain log_ai_flix("ai/flix");
#define DBG_AI_FLIX LOG_STREAM(debug, log_ai_flix)
#define LOG_AI_FLIX LOG_STREAM(info, log_ai_flix)
#define WRN_AI_FLIX LOG_STREAM(warn, log_ai_flix)
#define ERR_AI_FLIX LOG_STREAM(err, log_ai_flix)

#ifdef _MSC_VER
#pragma warning(push)
//silence "inherits via dominance" warnings
#pragma warning(disable:4250)
#endif

namespace ai {

namespace flix2_recruitment {

	recruitment::recruitment( rca_context &context , const config &cfg ):
		candidate_action(context, cfg){ 	}
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

	//first line
	out << std::setprecision(4);
	out << "          my/enemy";
	std::map<std::string, double> first_line = (table.begin())->second;
	for (std::map<std::string, double>::const_iterator i = first_line.begin();
			i != first_line.end(); ++i) {
		out << "|" << std::setw(15) << i->first << std::setw(3);
	}
	//other lines
	for (effectivness_table::const_iterator i = table.begin(); i != table.end();
			++i) {
		out << "\n" << std::setw(18) << i->first;
		for (std::map<std::string, double>::const_iterator j = (i->second).begin();
				j != (i->second).end(); ++j) {
			//out << "|      " << j->second << std::setw(13 - floor(log10(double(j->second))));
			out << "|" << std::setw(10) << j->second << std::setw(8);
		}
	}
	LOG_AI_FLIX<< "\n" << out.str() << "\n";
}

void recruitment::execute() {
		LOG_AI_FLIX << "Flix recruitment begin! \n";

		const std::vector<team> &teams = *resources::teams;

		const std::set<std::string> own_recruits = current_team().recruits();
		std::set<std::string> enemy_recruits;

		//for now we only use the recruit list of one enemy (the first found)
		//later one can think about including all enemies
		for(std::vector<team>::const_iterator i = teams.begin(); i != teams.end(); ++i) {
			if(current_team().is_enemy(i->side())){
				enemy_recruits = i->recruits();
			}
		}

		effectivness_table table;

		std::set<std::string>::const_iterator own_unit_s;
		std::set<std::string>::const_iterator enemy_unit_s;

		for(own_unit_s = own_recruits.begin(); own_unit_s != own_recruits.end(); ++own_unit_s){
			for(enemy_unit_s = enemy_recruits.begin(); enemy_unit_s != enemy_recruits.end(); ++enemy_unit_s){
				const unit_type *own_unit = unit_types.find(*own_unit_s);
				const unit_type *enemy_unit = unit_types.find(*enemy_unit_s);

				const double own_effectiveness_vs_enemy = average_resistance_against(*enemy_unit,*own_unit);
				const double enemy_effectiveness_vs_own = average_resistance_against(*own_unit, *enemy_unit);

				//weight in the cost difference
				//int cost_weight = 400 * (enemy_unit->cost() - own_unit->cost());
				//table[*own_unit_s][*enemy_unit_s] = compare_unit_types(*own_unit, *enemy_unit) + cost_weight;
				//table[*own_unit_s][*enemy_unit_s] = rand() % 20;

				table[*own_unit_s][*enemy_unit_s] = own_effectiveness_vs_enemy / own_unit->cost() - enemy_effectiveness_vs_own / enemy_unit->cost(); // * own_unit->cost() / enemy_unit->cost();
				//table[*own_unit_s][*enemy_unit_s] = own_effectiveness_vs_enemy - enemy_effectiveness_vs_own;
				LOG_AI_FLIX << *own_unit_s << " : " << *enemy_unit_s << " << " << own_effectiveness_vs_enemy << " - " << enemy_effectiveness_vs_own << " = " << table[*own_unit_s][*enemy_unit_s] << "\n";
			}
		}

		print_table(table);
		std::map<std::string, double>* strategy = findEquilibrium(table);
		recruit_result_ptr recruit_result;
		do{
			int random = rand() % 100;
			std::string recruit;
			for(std::map<std::string, double>::const_iterator i = strategy->begin(); i != strategy->end(); ++i){
				random -= (i->second * 100);
				if(random <= 0){
					recruit = i->first;
					break;
				}
			}
			recruit_result = execute_recruit_action(recruit);
			LOG_AI_FLIX << "Recruited " << recruit << std::endl;
		} while(recruit_result->is_ok());
	}

	recruitment::~recruitment()
	{
	}


	int recruitment::average_resistance_against(const unit_type& a, const unit_type& b) const
	{
		int weighting_sum = 0, defense = 0;
		const std::map<t_translation::t_terrain, size_t>& terrain =
			resources::game_map->get_weighted_terrain_frequencies();

		for (std::map<t_translation::t_terrain, size_t>::const_iterator j = terrain.begin(),
		     j_end = terrain.end(); j != j_end; ++j)
		{
			// Use only reachable tiles when computing the average defense.
		  if (a.movement_type().movement_cost(j->first) < movetype::UNREACHABLE) {
				defense += a.movement_type().defense_modifier(j->first) * j->second;
				weighting_sum += j->second;
			}
		}

		if (weighting_sum == 0) {
			// This unit can't move on this map, so just get the average weighted
			// of all available terrains. This still is a kind of silly
			// since the opponent probably can't recruit this unit and it's a static unit.
			for (std::map<t_translation::t_terrain, size_t>::const_iterator jj = terrain.begin(),
					jj_end = terrain.end(); jj != jj_end; ++jj)
			{
				defense += a.movement_type().defense_modifier(jj->first) * jj->second;
				weighting_sum += jj->second;
			}
		}

		if(weighting_sum != 0) {
			defense /= weighting_sum;
		} else {
			LOG_AI_FLIX << "The weighting sum is 0 and is ignored.\n";
		}

		LOG_AI_FLIX << "average defense of '" << a.id() << "': " << defense << "\n";

		int sum = 0, weight_sum = 0;

		// calculation of the average damage taken
		bool steadfast = a.has_ability_by_id("steadfast");
		bool poisonable = !a.musthave_status("unpoisonable");
		const std::vector<attack_type>& attacks = b.attacks();
		for (std::vector<attack_type>::const_iterator i = attacks.begin(),
		     i_end = attacks.end(); i != i_end; ++i)
		{
			int resistance = a.movement_type().resistance_against(*i);
			// Apply steadfast resistance modifier.
			if (steadfast && resistance < 100)
				resistance = std::max<int>(resistance * 2 - 100, 50);
			// Do not look for filters or values, simply assume 70% if CTH is customized.
			int cth = i->get_special_bool("chance_to_hit", true) ? 70 : defense;
			int weight = i->damage() * i->num_attacks();
			// if cth == 0 the division will do 0/0 so don't execute this part
			if (poisonable && cth != 0 && i->get_special_bool("poison", true)) {
				// Compute the probability of not poisoning the unit.
				int prob = 1;
				for (int j = 0; j < i->num_attacks(); ++j)
					prob = prob * (100 - cth) /100;
				// Assume poison works one turn.
				weight += game_config::poison_amount * (100 - prob) / 100;
			}
			sum += cth * resistance * weight * weight; // average damage * weight
			weight_sum += weight;
		}

		// normalize by HP
		sum /= std::max<int>(1,std::min<int>(a.hitpoints(),1000)); // avoid values really out of range

		// Catch division by zero here if the attacking unit
		// has zero attacks and/or zero damage.
		// If it has no attack at all, the ai shouldn't prefer
		// that unit anyway.
		if (weight_sum == 0) {
			return sum;
		}
		return sum/weight_sum;
	}



	/**
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

		//LOG_AI_FLIX << "average defense of '" << a.id() << "': " << defense_a << "\n";
		//LOG_AI_FLIX << "average defense of '" << b.id() << "': " << defense_b << "\n";

		int best_attack_damage = -99999;
		const attack_type * best_attack = NULL;

		// calculation of the average damage taken
		bool steadfast_a = a.has_ability_by_id("steadfast");
		bool poisonable_a = !a.musthave_status("unpoisonable");
		bool steadfast_b = b.has_ability_by_id("steadfast");
		bool poisonable_b = !b.musthave_status("unpoisonable");
		const std::vector<attack_type>& attacks_b = b.attacks();
		for (std::vector<attack_type>::const_iterator i = attacks_b.begin(),
		     i_end = attacks_b.end(); i != i_end; ++i)
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
			int damage_to_a = cth_a * resistance_a * plain_damage_to_a; // average damage * weight

			//now calculate the defense damage
			int max_defense_demage = 0;
			const attack_type * best_defense = NULL;
			const std::vector<attack_type>& attacks_a = a.attacks();
			for (std::vector<attack_type>::const_iterator j = attacks_a.begin(),
					     j_end = attacks_a.end(); j != j_end; ++j)
			{
				//check if defence weappon is of the same type as attack weappon
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
			//V3 damage_to_a = damage_to_a * a.cost() / b.cost();


			damage_to_a /= b.cost();
			max_defense_demage /= a.cost();

			//maybe the defender cannot fight back because he's dead.
			//max_defense_demage /= 1.5;

			int attack_damage = damage_to_a - max_defense_demage;

			if(attack_damage > best_attack_damage){
				best_attack_damage = attack_damage;
				best_attack = &(*i);
			}
		}

		LOG_AI_FLIX << "a:" << a.type_name() << " b:" << b.type_name() << " << Best_attack: " << ((best_attack) ? best_attack->name() : "#") << ": " << best_attack_damage << "\n";



		// Catch division by zero here if the attacking unit
		// has zero attacks and/or zero damage.
		// If it has no attack at all, the ai shouldn't prefer
		// that unit anyway.
//		if (weight_sum == 0) {
//			return sum;
//		}
		return best_attack_damage;
	}
	 **/
	int recruitment::compare_unit_types(const unit_type& a, const unit_type& b) const
	{
		const int a_effectiveness_vs_b = average_resistance_against(b,a);
		const int b_effectiveness_vs_a = average_resistance_against(a,b);

		LOG_AI_FLIX << "comparison of '" << a.id() << " vs " << b.id() << ": "
			<< a_effectiveness_vs_b << " - " << b_effectiveness_vs_a << " = "
			<< (a_effectiveness_vs_b - b_effectiveness_vs_a) << '\n';
		return a_effectiveness_vs_b - b_effectiveness_vs_a;
	}

	std::map<std::string, double>* recruitment::findEquilibrium(effectivness_table& table) const{

		std::stringstream s;

		/**
		 * Preparing some datastructures for Step 5
		 */

		//Because we want to keep the keys of table immutable we cannot simply exchange them
		//like in Step 5 suggested. Instead we keep the "exchangeable names" in separate maps.
		std::map<std::string, std::string> names_above;
		std::map<std::string, std::string> names_below;
		std::map<std::string, std::string> names_left;
		std::map<std::string, std::string> names_right;

		//iterating over all rows
		for(effectivness_table::const_iterator i = table.begin(); i != table.end(); ++i){

			names_left[i->first] = i->first;
			names_right[i->first] = "";
		}
		//iterating over first row
		std::map<std::string, double> first_row = (table.begin())->second;
		for(std::map<std::string, double>::const_iterator i = first_row.begin(); i != first_row.end(); ++i){

			names_above[i->first] = i->first;
			names_below[i->first] = "";
		}


		/**
		 * Step1: Add offset so all values are not negative.
		 */

		double min = 0;

		//iterating over all elements in the table
		for(effectivness_table::const_iterator i = table.begin(); i != table.end(); ++i){
			for(std::map<std::string, double>::const_iterator j = (i->second).begin(); j != (i->second).end(); ++j){
				if(j->second < min){
					min = j->second;
				}
			}
		}

		if(min < 0){
			//iterating over all elements in the table
			for(effectivness_table::iterator i = table.begin(); i != table.end(); ++i){
				for(std::map<std::string, double>::iterator j = (i->second).begin(); j != (i->second).end(); ++j){
					j->second = j->second - min;
				}
			}
		}

		LOG_AI_FLIX << "Table after Step 1: \n";
		print_table(table);

		/**
		 * Step 2: Adding cells, init D.
		 */

		//iterating over first row
		for(std::map<std::string, double>::const_iterator i = first_row.begin(); i != first_row.end(); ++i){
			table["_"][i->first] = -1;
		}

		//iterating over all lines
		for(effectivness_table::iterator i = table.begin(); i != table.end(); ++i){
			(i->second)["_"] = 1;
		}

		table["_"]["_"] = 0;

		double D = 1;

		LOG_AI_FLIX << "Table after Step 2: \n";
		print_table(table);
		LOG_AI_FLIX << "D = " << D << "\n";

		bool found;
		do{
			/**
			 * Step 3: Find pivot element
			 */

			//a map to save the smallest candidate pivot for each column
			//       columnname             rowname   pivot creteria
			std::map<std::string, std::pair<std::string, double> > column_mins;

			//iterate over columns
			for(std::map<std::string, double>::const_iterator i = first_row.begin(); i != first_row.end(); ++i){
				//The number at the foot of the column must be negative
				if(table["_"][i->first] >= 0){
					continue;
				}

				double column_min = INFINITY;
				std::string column_min_row_name;

				//iterate over rows
				for(effectivness_table::const_iterator j = table.begin(); j != table.end(); ++j){
					//candidate pivot must be positive
					if(j->first == "_" || table[j->first][i->first] <= 0){
						continue;
					}
					//candidate = -(r * c) / p
					double candidate = -(table[j->first]["_"] * table["_"][i->first]) / static_cast<double>(table[j->first][i->first]);

					if(candidate < column_min){
						column_min = candidate;
						column_min_row_name = j->first;
					}
				}//iterate over rows

				column_mins[i->first] = std::make_pair(column_min_row_name, column_min);
			}//iterate over columns

			//find Maximum in columnMins
			double max = - INFINITY;
			std::pair<std::string, std::string> pivot_index;
			for(std::map<std::string, std::pair<std::string, double> >::const_iterator i = column_mins.begin(); i != column_mins.end(); ++i){
				if((i->second).second > max){
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
			for(effectivness_table::iterator i = table.begin(); i != table.end(); ++i){
				for(std::map<std::string, double>::iterator j = (i->second).begin(); j != (i->second).end(); ++j){
					if(i->first == pivot_index.first || j->first == pivot_index.second){
						continue;
					}

					double N = j->second;
					double P = pivot;
					double R = table[pivot_index.first][j->first];
					double C = table[i->first][pivot_index.second];

					j->second = ((N * P) - (R * C)) / D;
				}
			} //iterating over each cell

			//Step 4.3 : Negating Column
			//iterating over column with pivot
			for(effectivness_table::iterator i = table.begin(); i != table.end(); ++i){
				if(i->first == pivot_index.first){
					continue;
				}
				(i->second)[pivot_index.second] = - (i->second)[pivot_index.second];
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

			//iterate over rows
			for(effectivness_table::const_iterator i = table.begin(); i != table.end(); ++i){
				if(table[i->first]["_"] < 0){
					found = false;
				}
			}

			//iterate over colums
			for(std::map<std::string, double>::const_iterator i = first_row.begin(); i != first_row.end(); ++i){
				if(table["_"][i->first] < 0){
					found = false;
				}
			}
		} while(!found);

		LOG_AI_FLIX << "Found Equilibrium!\n";

		//build Strategy
		std::map<std::string, double>* strategy = new std::map<std::string, double>();
		double sum = 0;

		//iterate over names_below
		for(std::map<std::string, std::string>::const_iterator i = names_below.begin(); i != names_below.end(); ++i){
			if(i->second != ""){
				(*strategy)[i->second] = table["_"][i->first];
				sum += table["_"][i->first];
			}
		}

		s.str("");
		s << "Strategy is: ";
		//normalize so sum of probabilities is 1
		for(std::map<std::string, double>::iterator i = strategy->begin(); i != strategy->end(); ++i){
			i->second /= sum;
			s << i->first << ": " << (i->second * 100) << "%  ";
		}
		LOG_AI_FLIX << s.str() << "\n";

		return strategy;
	}

}

}
